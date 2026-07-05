# TSan Port Phase 4: Hardening — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Commit the already-verified evidence from Phase 4's investigation: a repeated-fork stress harness proving R2's TSan fork-hook mechanism is stable across many sequential `fast_multithreaded_fork()` calls (Dimension A), and a documentation update closing PLAN.txt's D7/Q4 open question about the explicit-pthread_exit-at-scale residual (Dimension B) now that it's been reproduced and shown not to apply to McMini's production code.

**Architecture:** One new standalone test file extending Phase 2's harness architecture, plus a documentation-only edit to `PLAN.txt` resolving its own open question. No production code changes in this phase (confirmed unnecessary by the design's investigation).

**Tech Stack:** C11, ThreadSanitizer (`-fsanitize=thread`), the same `dlopen`/`__clone`/TSan-fiber techniques used throughout this port.

## Global Constraints

- Design reference: `docs/superpowers/specs/2026-07-05-tsan-port-phase4-hardening-design.md`.
- Baseline: branch `tsan-checkpoint-restart`, commit `9d389b2` (Phase 4 design spec, on top of prior phases).
- This project has no C unit-test harness (per `CLAUDE.md`); standalone tests are compiled and run directly with `gcc`, never via CMake.
- **ThreadSanitizer requires ASLR disabled in this sandbox.** Every TSan run in this plan uses `setarch -R` with `TSAN_OPTIONS="handle_segv=0 die_after_fork=0"`.
- No production code changes are in scope for this phase — both investigations (Dimension A and B) concluded no fix is needed. Do not add one.

---

### Task 1: Repeated-fork stress harness (Dimension A)

**Files:**
- Create: `test/tsan_support/test_repeated_fork_stress.c`

**Interfaces:** None consumed from other tasks. Produces no interface other tasks depend on — a standalone, self-contained stress test (compiled/run directly with `gcc -fsanitize=thread`, matching the `test/tsan_support/` convention established in Phases 1-3).

- [ ] **Step 1: Write the harness**

Create `test/tsan_support/test_repeated_fork_stress.c`:

```c
// Standalone host-side stress test. Not an mcmini model-checking target.
//
// Extends test_fastpath_fork_clone_fiber.c's architecture to stress-test R2's
// TSan fork-hook bracketing (__sanitizer_syscall_pre/post_impl_fork around
// _Fork()) across MANY repeated fast_multithreaded_fork() calls from the same
// long-lived process -- mirroring mc_template_thread_loop_forever()'s real
// while(1) structure (src/lib/template/loop.c:207-239), which never resets
// threadIdx between calls because threadInfos[] is a snapshot taken once and
// reused for every subsequent branch fork.
//
// Unlike test_fastpath_fork_clone_fiber.c (whose original threads run their
// workload once after being released, modeling a direct-restart-into-branch
// scenario), this harness's original ("template") threads check in once and
// park FOREVER -- matching DMTCP_RESTART_INTO_TEMPLATE's real "threads must
// wait forever" semantics, since the same getcontext() snapshot must remain
// valid and reusable across every one of the NUM_FORKS repeated forks below.
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/wait.h>

#ifdef __x86_64__
#include <asm/prctl.h>
#include <sys/prctl.h>
#else
#error "This standalone harness only covers __x86_64__; see dmtcp-callback.c for other architectures."
#endif

#ifndef NUM_WORKERS
#define NUM_WORKERS 3
#endif
#ifndef NUM_FORKS
#define NUM_FORKS 10
#endif
#define ITERS 100000

// ---- TSan externs, copied verbatim from src/lib/dmtcp-callback.c ----
extern void *__tsan_create_fiber(unsigned flags) __attribute__((weak));
extern void __tsan_switch_to_fiber(void *fiber, unsigned flags) __attribute__((weak));
extern void __sanitizer_syscall_pre_impl_fork(void) __attribute__((weak));
extern void __sanitizer_syscall_post_impl_fork(long res) __attribute__((weak));
extern int __clone(int (*fn)(void *), void *child_stack, int flags, void *arg,
                    ... /* pid_t *ptid, void *newtls, pid_t *ctid */);

// ---- struct threadinfo + TLS/descriptor helpers, copied verbatim from
// src/lib/dmtcp-callback.c (x86_64 branch only), matching Phase 1-2's
// accepted-duplication precedent (linking the real file is impractical). ----
struct threadinfo {
  ucontext_t context;
  unsigned long fs;
  unsigned long gs;
  pthread_t pthread_descriptor;
};
static struct threadinfo threadInfos[NUM_WORKERS];
static atomic_int threadIdx = 0;

static void getTLSPointer(struct threadinfo *ti) {
  assert(syscall(SYS_arch_prctl, ARCH_GET_FS, &ti->fs) == 0);
  assert(syscall(SYS_arch_prctl, ARCH_GET_GS, &ti->gs) == 0);
}
static void setTLSPointer(struct threadinfo *ti) {
  assert(syscall(SYS_arch_prctl, 2, ARCH_SET_FS, ti->fs) != 0);
  assert(syscall(SYS_arch_prctl, 2, ARCH_SET_GS, ti->gs) != 0);
}
static int pthreadDescriptorTidOffset(void) { return 720; }
static pid_t patchThreadDescriptor(pthread_t pthreadSelf) {
  int offset = pthreadDescriptorTidOffset();
  pid_t oldtid = *(pid_t *)((char *)pthreadSelf + offset);
  *(pid_t *)((char *)pthreadSelf + offset) = syscall(SYS_gettid);
  return oldtid;
}

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static long shared_counter = 0;
static sem_t sem_checkin, sem_park_forever, sem_done;

static void sem_wait_retry(sem_t *s) {
  while (sem_wait(s) != 0) /* EINTR */;
}

static int child_setcontext_fast(void *arg) {
  struct threadinfo *ti = arg;
  setTLSPointer(ti);
  patchThreadDescriptor(ti->pthread_descriptor);
  setcontext(&ti->context);
  return 0;
}

static void restart_child_threads_fast(void) {
  int maxThreadIdx = atomic_load(&threadIdx);
  for (int i = 0; i < maxThreadIdx; i++) {
    int clone_flags = (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SYSVSEM
                       | CLONE_SIGHAND | CLONE_THREAD
                       | CLONE_SETTLS | CLONE_PARENT_SETTID
                       | CLONE_CHILD_CLEARTID);
    void *stack = malloc(0x10000) + 0x10000 - 128; // 64 KB, intentionally leaked
    int offset = pthreadDescriptorTidOffset();
    pid_t *ctid = (pid_t *)((char *)threadInfos[i].pthread_descriptor + offset);
    pid_t *ptid = ctid;
    __clone(child_setcontext_fast, stack, clone_flags,
            (void *)&threadInfos[i], ptid, threadInfos[i].fs, ctid);
  }
}

static pid_t fast_multithreaded_fork(void) {
  pid_t _Fork();
  if (__sanitizer_syscall_pre_impl_fork != NULL) {
    __sanitizer_syscall_pre_impl_fork();
  }
  int childpid = _Fork();
  if (__sanitizer_syscall_post_impl_fork != NULL) {
    __sanitizer_syscall_post_impl_fork(childpid);
  }
  if (childpid == 0) { // child process
    if (__tsan_switch_to_fiber != NULL) {
      __tsan_switch_to_fiber(__tsan_create_fiber(0), 0);
    }
    restart_child_threads_fast();
  }
  return childpid;
}

// Mirrors the ORIGINAL (template-process) thread's role: check in once, then
// park FOREVER -- see the file header comment for why this differs from
// test_fastpath_fork_clone_fiber.c's "release and run once" model.
static void *worker(void *arg) {
  (void)arg;
  pid_t orig_pid = getpid();
  int idx = atomic_fetch_add(&threadIdx, 1);
  struct threadinfo *ti = &threadInfos[idx];
  memset(ti, 0, sizeof(*ti));
  ti->pthread_descriptor = pthread_self();
  getTLSPointer(ti);

  int rc = getcontext(&ti->context);
  assert(rc == 0);

  if (getpid() == orig_pid) {
    sem_post(&sem_checkin);
    sem_wait_retry(&sem_park_forever); // never released
    return NULL; // not reached
  }

  // Resumed via __clone()+setcontext in a forked branch child.
  if (__tsan_switch_to_fiber != NULL) {
    __tsan_switch_to_fiber(__tsan_create_fiber(0), 0);
  }
  for (int i = 0; i < ITERS; i++) {
    pthread_mutex_lock(&mtx);
    shared_counter++;
    pthread_mutex_unlock(&mtx);
  }
  sem_post(&sem_done);
  sem_wait_retry(&sem_park_forever); // park; process _exit()s shortly
  return NULL;
}

int main(void) {
  sem_init(&sem_checkin, 0, 0);
  sem_init(&sem_park_forever, 0, 0);
  sem_init(&sem_done, 0, 0);

  pthread_t th[NUM_WORKERS];
  for (int i = 0; i < NUM_WORKERS; i++) {
    pthread_create(&th[i], NULL, worker, NULL);
  }
  for (int i = 0; i < NUM_WORKERS; i++) {
    sem_wait(&sem_checkin);
  }

  int failures = 0;
  for (int iter = 0; iter < NUM_FORKS; iter++) {
    pid_t pid = fast_multithreaded_fork();
    if (pid == 0) {
      for (int i = 0; i < NUM_WORKERS; i++) {
        sem_wait(&sem_done);
      }
      int ok = (shared_counter == NUM_WORKERS * ITERS);
      fprintf(stderr, "[CHILD iter=%d pid=%d] shared_counter=%ld (expected %d) %s\n",
              iter, getpid(), shared_counter, NUM_WORKERS * ITERS, ok ? "OK" : "MISMATCH");
      _exit(ok ? 0 : 1);
    } else {
      int status;
      waitpid(pid, &status, 0);
      int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
      fprintf(stderr, "[PARENT iter=%d] child pid=%d exited=%d code=%d signaled=%d %s\n",
              iter, pid, WIFEXITED(status), WEXITSTATUS(status), WIFSIGNALED(status),
              ok ? "OK" : "FAIL");
      if (!ok) failures++;
    }
  }

  fprintf(stderr, "%s: %d/%d iterations passed\n",
          failures == 0 ? "ALL PASSED" : "SOME FAILED", NUM_FORKS - failures, NUM_FORKS);
  return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Build and run — verify all 10 iterations pass**

Run:
```bash
gcc -fsanitize=thread -g -O1 -Wall -pthread -o /tmp/test_repeated_fork_stress test/tsan_support/test_repeated_fork_stress.c -fsanitize=thread -pthread
TSAN_OPTIONS="handle_segv=0 die_after_fork=0" setarch -R /tmp/test_repeated_fork_stress
echo "exit: $?"
```

Expected: compiles clean, prints 10 `[CHILD iter=N ...] shared_counter=300000 (expected 300000) OK` / `[PARENT iter=N] ... exited=1 code=0 signaled=0 OK` pairs (N = 0..9), ends with `ALL PASSED: 10/10 iterations passed`, `exit: 0`. No `CHECK failed`, no SEGV, no hang.

- [ ] **Step 3: Commit**

```bash
git add test/tsan_support/test_repeated_fork_stress.c
git commit -m "Add repeated-fork stress harness proving R2 stability across sequential branches"
```

---

### Task 2: Close PLAN.txt's D7/Q4 open question (Dimension B)

**Files:**
- Modify: `PLAN.txt:163-165` (D7 entry)
- Modify: `PLAN.txt:252-253` (Q4 entry)

**Interfaces:** None — this is a documentation-only change resolving PLAN.txt's own open question with the finding from this phase's investigation (already reproduced and analyzed in the design spec; no new investigation happens in this task).

- [ ] **Step 1: Update the D7 entry**

In `PLAN.txt`, current lines 163-165:

```
D7. The residual explicit-pthread_exit()-at-scale issue (standalone) may or may not
    matter here, since branch threads are model-checker-scheduled (rarely many
    simultaneous real pthread_exit calls).  Decide whether to require robustness.
```

Replace with:

```
D7. RESOLVED (Phase 4): the residual explicit-pthread_exit()-at-scale issue does
    NOT matter here. Reproduced it in the standalone package directly (built
    test_exit.c with NUM_WORKERS=8, ran 30x under real TSan: 1/30 runs crashed
    with "stack smashing detected", matching the vendor's own ~1/30 rate) --
    confirming the residual is real, but it is specific to the vendor's own
    join/exit shim (tsan_join_shim.c), which performs a REAL pthread_exit() +
    REAL pthread_join() on a recreated thread. McMini's production
    mc_pthread_exit()/mc_pthread_join() never do this: TARGET_BRANCH* mode is
    always a shared-memory mailbox handshake with the mcmini scheduler (see
    Phase 3's R5 investigation). No McMini robustness work needed.
```

- [ ] **Step 2: Update the Q4 entry**

In `PLAN.txt`, current lines 252-253:

```
  Q4. Do we require explicit-pthread_exit()-at-scale robustness (D7), or document
      it as a limitation as the standalone does?
```

Replace with:

```
  Q4. RESOLVED (Phase 4): no robustness work required -- see D7. Documented as a
      vendor-package-only limitation that McMini's production code structurally
      cannot hit, matching the standalone's own documented-not-fixed approach.
```

- [ ] **Step 3: Verify the edits**

Run:
```bash
grep -n "D7\." PLAN.txt
grep -n "Q4\." PLAN.txt
```

Expected: both show the "RESOLVED (Phase 4)" text from Steps 1-2.

- [ ] **Step 4: Commit**

```bash
git add PLAN.txt
git commit -m "Close PLAN.txt D7/Q4: explicit-pthread_exit-at-scale residual doesn't apply to McMini"
```

---

### Task 3: End-to-end verification against real TSan example targets under DMTCP (environment-gated)

**Precondition:** this task requires the DMTCP toolchain (`dmtcp_launch`, `dmtcp_restart`) installed and on `PATH`, and the project built with `MCMINI_WITH_DMTCP=ON`. As of this plan's writing, this environment has neither (same gap every prior phase's Task 3 hit). If unavailable when this task is reached, stop after Task 2, report Tasks 1-2 complete and committed, and hand this task off — do not mark it done without actually running it.

**Files:** none (this task runs commands and observes output; it does not change source).

- [ ] **Step 1: Reconfigure and rebuild with DMTCP enabled**

```bash
cmake -S . -B build -DMCMINI_WITH_DMTCP=ON
cmake --build build --target libmcmini
```

Expected: build succeeds and produces `build/libmcmini.so`.

- [ ] **Step 2: Build TSan-instrumented example targets**

Per PLAN.txt Phase 4 ("a couple of example targets"), build at least two of the existing `src/examples/*` targets with ThreadSanitizer:

```bash
gcc -fsanitize=thread -g -pthread -o /tmp/producer-consumer-tsan src/examples/producer-consumer.c
gcc -fsanitize=thread -g -pthread -o /tmp/cv-test-tsan src/examples/cv-test.c
```

Expected: both compile with no errors.

- [ ] **Step 3: Record checkpoints**

From the directory containing `libmcmini.so`:

```bash
cd build
TSAN_OPTIONS="handle_segv=0 die_after_fork=0" \
  dmtcp_launch --disable-alloc-plugin -i 5 --with-plugin "$PWD/libmcmini.so" \
  /tmp/producer-consumer-tsan
```

Let it run for at least one checkpoint interval, stop it (Ctrl-C) once a `ckpt_*.dmtcp` file appears. Repeat for `/tmp/cv-test-tsan`.

Expected: a `ckpt_producer-consumer-tsan_*.dmtcp` and a `ckpt_cv-test-tsan_*.dmtcp` file are each created.

- [ ] **Step 4: Restart under `mcmini` with `--multithreaded-fork`, exploring multiple branches**

```bash
setarch -R ./mcmini --from-checkpoint ckpt_producer-consumer-tsan_*.dmtcp --multithreaded-fork /tmp/producer-consumer-tsan
setarch -R ./mcmini --from-checkpoint ckpt_cv-test-tsan_*.dmtcp --multithreaded-fork /tmp/cv-test-tsan
```

Expected (this phase's pass criterion): for each target, model checking runs to completion (or explores a substantial number of branches without crashing) with no SIGSEGV or `ThreadSanitizer: CHECK failed`, and results match the same target run without TSan-instrumentation, per PLAN.txt section 7's own pass criteria. This is the first real test of the "multiple sequential branches" concern (Dimension A of this phase) under the actual DMTCP+mcmini machinery, since Task 1's standalone harness could only test it in isolation.

- [ ] **Step 5: Record the observed outcome**

No commit for this task (no source changes; target binaries live in `/tmp`, not the repo). If the pass criterion in Step 4 is met, note it in the PR/handoff description; if not, capture the log output and hand off to further investigation rather than attempting a fix here.
