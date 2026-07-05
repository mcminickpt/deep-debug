# TSan Port Phase 2 (R2+R3+R4): fork hooks, __clone, fiber switching — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the two remaining gaps in libmcmini's TSan `multithreaded_fork` port — R3 (`__clone` instead of the public `clone()`) and the R4 remainder (a fresh TSan fiber for the *forking* thread, not just recreated threads) — and prove the fix with a standalone test harness that reproduces the real ThreadSanitizer failure before the fix and passes cleanly after.

**Architecture:** Two small, mechanical edits to `src/lib/dmtcp-callback.c` (R2 and R4's recreated-thread half are already committed from Phase 0/1). A new standalone test harness, `test/tsan_support/test_fastpath_fork_clone_fiber.c`, mirrors `dmtcp-callback.c`'s actual getcontext-direct-call resumption mechanism (not the vendor package's signal-based one) and is built/run twice: once in a "buggy" configuration (public `clone()`, no forking-thread fiber switch) to capture the real TSan `CHECK failed` this phase fixes, then again after applying the identical fix, to confirm it passes.

**Tech Stack:** C11, CMake, ThreadSanitizer (`-fsanitize=thread`), glibc `__clone`/fiber APIs, POSIX threads/semaphores/ucontext.

## Global Constraints

- Build uses `-Wall -Werror` (`CMakeLists.txt:81`) — the production build must stay warning-free.
- The TLS/pthread-descriptor offset helpers (`pthreadDescriptorTidOffset()` etc.) are only defined for `__x86_64__` in the standalone harness (matching Phase 1's `test_tid_from_descriptor_offset.c` precedent); the harness guards this with `#error` on other architectures.
- **ThreadSanitizer requires ASLR disabled in this sandbox.** A bare `-fsanitize=thread` binary fails immediately with `FATAL: ThreadSanitizer: unexpected memory mapping`; running it via `setarch -R ./binary` fixes this (confirmed during design). Every TSan run in this plan uses `setarch -R`.
- Every TSan run in this plan sets `TSAN_OPTIONS="handle_segv=0 die_after_fork=0"` (matching the vendor package's own Makefile and PLAN.txt section 7).
- Design reference: `docs/superpowers/specs/2026-07-04-tsan-port-phase2-r2-r3-r4-design.md`.
- Baseline: branch `tsan-record-thread-fix`, commit `3664e7a` (Phase 2 design spec, on top of the Phase 0+1 work already merged).
- R2 (fork-hook bracketing of `_Fork()`) and R4's recreated-thread fiber switch are **already committed** (`src/lib/dmtcp-callback.c:236-242` and `:304-315` respectively) — do not modify them; this plan only adds the two remaining pieces.

---

### Task 1: Standalone test harness proving R3 + R4-remainder under real TSan

**Files:**
- Create: `test/tsan_support/test_fastpath_fork_clone_fiber.c`

**Interfaces:** None consumed from other tasks. Produces no interface other tasks depend on — this is a standalone, self-contained host-side test (compiled/run directly with `gcc`, not via CMake, per this project's lack of a C unit-test harness — see `CLAUDE.md`).

This task is unusual in that the "RED" state is achieved with a compile-time flag (`-DMTF_BUGGY`) rather than a missing function — the harness is fully self-contained from Step 1, and Step 2 proves it fails in the *buggy* configuration before Step 4 proves the *default* (fixed) configuration passes.

- [ ] **Step 1: Write the harness (default = fixed; `-DMTF_BUGGY` = buggy)**

Create `test/tsan_support/test_fastpath_fork_clone_fiber.c`:

```c
// Standalone host-side test harness. Not an mcmini model-checking target.
//
// Mirrors dmtcp-callback.c's ACTUAL fast-path resumption mechanism (each
// thread calls getcontext() directly, as a plain function call -- no signal
// handler, unlike the multithreaded-fork-tsan-2.0 standalone package) to
// exercise R2 (TSan fork hooks around _Fork()), R3 (__clone instead of the
// public clone()), and R4 (fresh TSan fiber for the forking thread AND each
// recreated thread) under real ThreadSanitizer.
//
// BUGGY MODE (for RED evidence): compile with -DMTF_BUGGY to use the public
// clone() (no R3) and skip the forking-thread fiber switch (no R4 remainder),
// reproducing the failure these two fixes exist to prevent.
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
#define ITERS 100000

// ---- TSan externs, copied verbatim from src/lib/dmtcp-callback.c ----
extern void *__tsan_create_fiber(unsigned flags) __attribute__((weak));
extern void __tsan_switch_to_fiber(void *fiber, unsigned flags)
    __attribute__((weak));
extern void __sanitizer_syscall_pre_impl_fork(void) __attribute__((weak));
extern void __sanitizer_syscall_post_impl_fork(long res) __attribute__((weak));

// libc's internal clone (NOT intercepted by libtsan, unlike public clone()).
extern int __clone(int (*fn)(void *), void *child_stack, int flags, void *arg,
                    ... /* pid_t *ptid, void *newtls, pid_t *ctid */);

// ---- struct threadinfo + TLS/descriptor helpers, copied verbatim from
// src/lib/dmtcp-callback.c (x86_64 branch only) for fidelity to production,
// including the "syscall(SYS_arch_prctl, 2, ARCH_SET_FS, ...)" call already
// proven correct by the multithreaded-fork-tsan-2.0 standalone package. ----
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

// ---- Harness state ----
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static long shared_counter = 0;
static sem_t sem_checkin, sem_release, sem_done, sem_park;

static void sem_wait_retry(sem_t *s) {
  while (sem_wait(s) != 0) /* EINTR */;
}

// ---- child_setcontext_fast(), copied verbatim in structure from
// src/lib/dmtcp-callback.c ----
static int child_setcontext_fast(void *arg) {
  struct threadinfo *ti = arg;
  setTLSPointer(ti);
  patchThreadDescriptor(ti->pthread_descriptor);
  setcontext(&ti->context); // never returns
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
#ifdef MTF_BUGGY
    // R3 NOT applied: public clone() is intercepted by libtsan and treated
    // as a fork, corrupting its thread-slot state for this CLONE_THREAD clone.
    clone(child_setcontext_fast, stack, clone_flags,
          (void *)&threadInfos[i], ptid, threadInfos[i].fs, ctid);
#else
    // R3: libc's raw __clone, not intercepted by libtsan.
    __clone(child_setcontext_fast, stack, clone_flags,
            (void *)&threadInfos[i], ptid, threadInfos[i].fs, ctid);
#endif
  }
}

// ---- fast_multithreaded_fork(), copied in structure from
// src/lib/dmtcp-callback.c ----
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
#ifndef MTF_BUGGY
    // R4 remainder: the forking thread keeps its inherited (fork-copied) TSan
    // ThreadState, whose shadow call stack starts at the parent's fork-time
    // depth and can overflow as this thread keeps running. Fresh fiber too.
    if (__tsan_switch_to_fiber != NULL) {
      __tsan_switch_to_fiber(__tsan_create_fiber(0), 0);
    }
#endif
    restart_child_threads_fast();
  }
  return childpid;
}

// ---- worker thread: mirrors thread_handle_after_dmtcp_restart()'s
// getcontext-direct-call mechanism (no signal handler). ----
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
    // Still in the original process (pre-fork): check in, then block until
    // released (mirrors production's cond_wait parking; a plain semaphore is
    // sufficient here since R1's mode-machinery is out of scope for R2/R3/R4).
    sem_post(&sem_checkin);
    sem_wait_retry(&sem_release);
  } else {
    // Resumed via __clone()+setcontext in the forked child: give this
    // TSan-invisible OS thread a valid ThreadState before any instrumented
    // call below. (Already-proven R4 half; unchanged by MTF_BUGGY.)
    if (__tsan_switch_to_fiber != NULL) {
      __tsan_switch_to_fiber(__tsan_create_fiber(0), 0);
    }
  }

  // TSan-intercepted work: locked shared write, exercised both by the
  // original threads (parent) and the recreated threads (child).
  for (int i = 0; i < ITERS; i++) {
    pthread_mutex_lock(&mtx);
    shared_counter++;
    pthread_mutex_unlock(&mtx);
  }
  sem_post(&sem_done);
  sem_wait_retry(&sem_park); // park forever; R5 join/exit shims out of scope
  return NULL;
}

int main(void) {
  sem_init(&sem_checkin, 0, 0);
  sem_init(&sem_release, 0, 0);
  sem_init(&sem_done, 0, 0);
  sem_init(&sem_park, 0, 0);

  pthread_t th[NUM_WORKERS];
  for (int i = 0; i < NUM_WORKERS; i++) {
    pthread_create(&th[i], NULL, worker, NULL);
  }
  for (int i = 0; i < NUM_WORKERS; i++) {
    sem_wait(&sem_checkin);
  }

  fprintf(stderr, "[main pid=%d] all workers checked in; forking...\n", getpid());
  pid_t pid = fast_multithreaded_fork();
  const char *who = (pid == 0) ? "CHILD" : "PARENT";
  fprintf(stderr, "[%s pid=%d] returned from fast_multithreaded_fork\n", who, getpid());

  if (pid > 0) {
    // PARENT: release the still-parked original workers.
    for (int i = 0; i < NUM_WORKERS; i++) {
      sem_post(&sem_release);
    }
  }
  // CHILD: recreated threads proceed on their own (see worker()'s else branch).

  for (int i = 0; i < NUM_WORKERS; i++) {
    sem_wait(&sem_done);
  }
  fprintf(stderr, "[%s pid=%d] shared_counter=%ld (expected %d)\n",
          who, getpid(), shared_counter, NUM_WORKERS * ITERS);

  if (pid > 0) {
    int status;
    waitpid(pid, &status, 0);
    fprintf(stderr, "[PARENT] child: exited=%d code=%d signaled=%d\n",
            WIFEXITED(status), WEXITSTATUS(status), WIFSIGNALED(status));
  }
  fprintf(stderr, "[%s pid=%d] done, _exit(0)\n", who, getpid());
  _exit(0);
}
```

- [ ] **Step 2: Build and run the BUGGY configuration — verify it fails (RED)**

Run:
```bash
gcc -DMTF_BUGGY -fsanitize=thread -g -O1 -Wall -pthread -o /tmp/mtf_fastpath_buggy test/tsan_support/test_fastpath_fork_clone_fiber.c -fsanitize=thread -pthread
TSAN_OPTIONS="handle_segv=0 die_after_fork=0" setarch -R /tmp/mtf_fastpath_buggy
echo "exit: $?"
```

Expected: compiles clean, then FAILS with output containing (one line per recreated thread):
```
ThreadSanitizer: CHECK failed: tsan_rtl.cpp:253 "((!thr->slot)) != (0)" (0x0, 0x0) (tid=...)
```
and `[PARENT] child: exited=1 code=66 signaled=0` (the child crashes; only the parent's `shared_counter=300000 (expected 300000)` line appears — the child never reaches its own). This is `ForkChildAfter` in libtsan's fork pipeline choking on the intercepted public `clone()` — exactly the R3 failure mode this phase fixes. `exit: 0` refers to the harness process itself (it doesn't propagate the child's crash as its own exit code; the crash is visible in the printed child status and the CHECK-failed lines).

- [ ] **Step 3: No code change needed — Step 1 already wrote the fixed (default) path**

The `#ifdef MTF_BUGGY` / `#else` branches in Step 1's file already contain both configurations. There is no separate "fix" edit to make in the harness itself; Step 4 simply compiles without `-DMTF_BUGGY`.

- [ ] **Step 4: Build and run the default (fixed) configuration — verify it passes (GREEN)**

Run:
```bash
gcc -fsanitize=thread -g -O1 -Wall -pthread -o /tmp/mtf_fastpath_fixed test/tsan_support/test_fastpath_fork_clone_fiber.c -fsanitize=thread -pthread
TSAN_OPTIONS="handle_segv=0 die_after_fork=0" setarch -R /tmp/mtf_fastpath_fixed
echo "exit: $?"
```

Expected: compiles clean, prints (pids will differ):
```
[main pid=...] all workers checked in; forking...
[PARENT pid=...] returned from fast_multithreaded_fork
[CHILD pid=...] returned from fast_multithreaded_fork
[CHILD pid=...] shared_counter=300000 (expected 300000)
[CHILD pid=...] done, _exit(0)
[PARENT pid=...] shared_counter=300000 (expected 300000)
[PARENT] child: exited=1 code=0 signaled=0
[PARENT pid=...] done, _exit(0)
```
No `CHECK failed`, no SEGV. `exit: 0`. Run it 3 times in a row to confirm it isn't flaky:
```bash
for i in 1 2 3; do TSAN_OPTIONS="handle_segv=0 die_after_fork=0" setarch -R /tmp/mtf_fastpath_fixed 2>&1 | grep -E "shared_counter|CHECK failed|exited"; done
```
Expected: all 3 runs show both `shared_counter=300000 (expected 300000)` lines and `exited=1 code=0 signaled=0`, no `CHECK failed` in any run.

- [ ] **Step 5: Commit**

```bash
git add test/tsan_support/test_fastpath_fork_clone_fiber.c
git commit -m "Add standalone harness proving R3 (__clone) + R4-remainder (forker fiber) under TSan"
```

---

### Task 2: Apply R3 + R4-remainder to production (`src/lib/dmtcp-callback.c`)

**Files:**
- Modify: `src/lib/dmtcp-callback.c:37-51` (add `__clone` extern next to the existing weak TSan externs)
- Modify: `src/lib/dmtcp-callback.c:186-208` (`restart_child_threads_fast()`: swap `clone()` for `__clone()`)
- Modify: `src/lib/dmtcp-callback.c:273-275` (`fast_multithreaded_fork()`'s child branch: add the forking-thread fiber switch)

**Interfaces:** None — this task applies, verbatim, the exact fix already proven in Task 1's harness to the real production functions of the same name and structure. No new functions are introduced.

- [ ] **Step 1: Add the `__clone` extern declaration**

In `src/lib/dmtcp-callback.c`, current lines 50-51:

```c
extern void __sanitizer_syscall_pre_impl_fork(void) __attribute__((weak));
extern void __sanitizer_syscall_post_impl_fork(long res) __attribute__((weak));
```

Add immediately after:

```c

// libc's internal clone (NOT intercepted by libtsan, unlike the public
// clone()). libtsan's clone() interceptor treats every call as a fork
// (ForkChildAfter), which corrupts its thread-slot state for the
// CLONE_THREAD clone restart_child_threads_fast() performs below. __clone is
// not intercepted, so it performs the raw thread creation; the recreated
// thread's own fiber switch (see thread_handle_after_dmtcp_restart()) then
// gives it a valid TSan ThreadState.
extern int __clone(int (*fn)(void *), void *child_stack, int flags,
                    void *arg, ... /* pid_t *ptid, void *newtls, pid_t *ctid */);
```

- [ ] **Step 2: Swap `clone()` for `__clone()` in `restart_child_threads_fast()`**

In `src/lib/dmtcp-callback.c`, current lines 202-206:

```c
    // For more insight, read 'man set_tid_address'.
    clone(child_setcontext_fast,
                      stack,
                      clone_flags,
                      (void *)&threadInfos[i], ptid, threadInfos[i].fs, ctid);
```

Replace with:

```c
    // For more insight, read 'man set_tid_address'.
    // R3: use libc's raw __clone, not the public clone() (see the __clone
    // extern declaration above for why).
    __clone(child_setcontext_fast,
                      stack,
                      clone_flags,
                      (void *)&threadInfos[i], ptid, threadInfos[i].fs, ctid);
```

- [ ] **Step 3: Add the forking-thread fiber switch in `fast_multithreaded_fork()`**

In `src/lib/dmtcp-callback.c`, current lines 273-275:

```c
  if (childpid == 0) { // child process
    restart_child_threads_fast();
  }
```

Replace with:

```c
  if (childpid == 0) { // child process
    // R4 (remainder): the forking thread keeps its inherited (fork-copied)
    // TSan ThreadState, whose shadow call stack starts at the parent's
    // fork-time depth and can overflow as this thread keeps running. Switch
    // it onto a fresh fiber too, mirroring the recreated-thread fiber switch
    // in thread_handle_after_dmtcp_restart(). Weak symbol: a no-op for
    // non-TSan targets.
    if (__tsan_switch_to_fiber != NULL) {
      __tsan_switch_to_fiber(__tsan_create_fiber(0), 0);
    }
    restart_child_threads_fast();
  }
```

- [ ] **Step 4: Rebuild and verify**

Run:
```bash
cmake --build build --target libmcmini
```

Expected: `[100%] Built target libmcmini` with no warnings (build uses `-Wall -Werror`).

- [ ] **Step 5: Commit**

```bash
git add src/lib/dmtcp-callback.c
git commit -m "Apply R3 (__clone) and R4 remainder (forker fiber) to fast_multithreaded_fork"
```

---

### Task 3: End-to-end verification against a real TSan target under DMTCP (environment-gated)

**Precondition:** this task requires the DMTCP toolchain (`dmtcp_launch`, `dmtcp_restart`) installed and on `PATH`, and the project built with `MCMINI_WITH_DMTCP=ON`. As of this plan's writing, this environment has neither (same gap Phase 1's Task 3 hit). If unavailable when this task is reached, stop after Task 2, report Tasks 1-2 complete and committed, and hand this task off — do not mark it done without actually running it.

**Files:** none (this task runs commands and observes output; it does not change source).

- [ ] **Step 1: Reconfigure and rebuild with DMTCP enabled**

```bash
cmake -S . -B build -DMCMINI_WITH_DMTCP=ON
cmake --build build --target libmcmini
```

Expected: build succeeds and produces `build/libmcmini.so`.

- [ ] **Step 2: Build a TSan-instrumented example target**

```bash
cd build/src/examples
gcc -fsanitize=thread -g -pthread -o producer-consumer-tsan ../../../src/examples/producer-consumer.c
cd -
```

Expected: `producer-consumer-tsan` binary produced with no compile errors.

- [ ] **Step 3: Record a checkpoint**

From the directory containing `libmcmini.so`:

```bash
cd build
TSAN_OPTIONS="handle_segv=0 die_after_fork=0" \
  dmtcp_launch --disable-alloc-plugin -i 5 --with-plugin "$PWD/libmcmini.so" \
  ./src/examples/producer-consumer-tsan
```

Let it run for at least one checkpoint interval (5s), then stop it (Ctrl-C) once a `ckpt_*.dmtcp` file appears in the current directory.

Expected: a `ckpt_producer-consumer-tsan_*.dmtcp` file is created.

- [ ] **Step 4: Restart under `mcmini` with `--multithreaded-fork`**

```bash
setarch -R ./mcmini --from-checkpoint ckpt_producer-consumer-tsan_*.dmtcp --multithreaded-fork ./src/examples/producer-consumer-tsan
```

Expected (this phase's pass criterion, per `PLAN.txt` Phase 2): the branch child runs and reaches the model-checker handshake without SIGSEGV or `ThreadSanitizer: CHECK failed` — the analog of Task 1's standalone "park" test passing, but through the real DMTCP-restart path. Full model-checking progress/completion is not required here (R5 join/exit shims are Phase 3); reaching the handshake without a crash is the pass criterion.

- [ ] **Step 5: Record the observed outcome**

No commit for this task (no source changes). If the pass criterion in Step 4 is met, note it in the PR/handoff description; if not, capture the log output and hand off to Phase 3 planning rather than attempting further fixes here (out of scope for this phase).
