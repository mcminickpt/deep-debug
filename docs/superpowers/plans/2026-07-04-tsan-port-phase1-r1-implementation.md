# TSan Port Phase 1 (R1): Exclude TSan-internal Threads from the Restart Barrier — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `template_thread()`'s DMTCP-restart thread-count barrier correctly exclude ThreadSanitizer's internal background thread, so restarting a `-fsanitize=thread` target under `--multithreaded-fork` no longer hangs waiting for a semaphore post that thread will never send.

**Architecture:** Two small, additive changes to `src/lib/dmtcp-callback.c`, backed by a new tiny helper module `src/lib/tsan_support.c`. `tsan_support.c` provides `thread_blocks_signal()`, a `/proc`-based probe that identifies threads with a specific signal blocked (how libtsan's background thread can be recognized, since it blocks all signals at creation). `dmtcp-callback.c` gains a read-only `get_tid_from_pthread_descriptor()` helper (a non-mutating sibling of the existing `patchThreadDescriptor()`) and uses both to replace `template_thread()`'s blanket `thread_count -= 2` with explicit per-tid classification (self / checkpoint thread / TSan-internal / countable).

**Tech Stack:** C11, CMake, glibc/Linux `/proc` filesystem, POSIX threads and signals. No new external dependencies.

## Global Constraints

- Build uses `-Wall -Werror` (`CMakeLists.txt:81`) — every new file must compile warning-free.
- C11 atomics are required to compile libmcmini (`include/mcmini/spy/checkpointing/record.h:7-8`) — not directly relevant to this plan's new code, but any new header included from that translation unit must not break this.
- `pthreadDescriptorTidOffset()` (and thus the new `get_tid_from_pthread_descriptor()`) only has defined values for `__x86_64__`, `__aarch64__`, and `__riscv` (`src/lib/dmtcp-callback.c:119-129`). This plan's standalone test only covers `__x86_64__` (the architecture of this dev machine); it is not a substitute for testing the other architectures.
- Design reference: `docs/superpowers/specs/2026-07-04-tsan-port-phase1-r1-design.md`. Follow it for rationale; this plan follows it for file placement and interfaces.
- Baseline: branch `tsan-multithreaded-fork-port`, commit `4d78676` (spec) on top of `b38da82` (Phase 0 cleanup).

---

### Task 1: `thread_blocks_signal()` in a new `tsan_support` module

**Files:**
- Create: `include/mcmini/spy/checkpointing/tsan_support.h`
- Create: `src/lib/tsan_support.c`
- Modify: `CMakeLists.txt:68-77` (add the new source file to `LIBMCMINI_C_SRC`)
- Test: `test/tsan_support/test_thread_blocks_signal.c` (standalone host-side unit test — compiled and run directly with `gcc`, not through CMake or as an mcmini model-checking target; this project has no C unit-test harness yet, per `CLAUDE.md`)

**Interfaces:**
- Produces: `int thread_blocks_signal(pid_t tid, int signo);` — declared in `tsan_support.h`, implemented in `tsan_support.c`. Returns nonzero if thread `tid` currently has signal `signo` blocked (per its `/proc/self/task/<tid>/status` `SigBlk` mask), zero otherwise (including if `tid` no longer exists). Consumed by Task 2.

- [ ] **Step 1: Write the header and the failing test**

Create `include/mcmini/spy/checkpointing/tsan_support.h`:

```c
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/**
 * Returns nonzero if thread `tid` currently has signal `signo` blocked, per
 * the "SigBlk" field of /proc/self/task/<tid>/status. Returns 0 if `tid`
 * cannot be inspected (e.g. it has already exited).
 */
int thread_blocks_signal(pid_t tid, int signo);

#ifdef __cplusplus
}
#endif
```

Create `test/tsan_support/test_thread_blocks_signal.c`:

```c
// Standalone host-side unit test for thread_blocks_signal(). Not an mcmini
// model-checking target: compile and run directly (see the command in the
// implementation plan / commit message), not through CMake.
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "mcmini/spy/checkpointing/tsan_support.h"

static sem_t ready;
static pid_t worker_tid;

static void *worker(void *arg) {
  (void)arg;
  worker_tid = (pid_t)syscall(SYS_gettid);

  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &set, NULL);

  sem_post(&ready);

  for (;;) {
    pause();  // cancellation point; SIGUSR1 stays blocked the whole time
  }
  return NULL;
}

int main(void) {
  int rc = sem_init(&ready, 0, 0);
  assert(rc == 0);

  pthread_t t;
  rc = pthread_create(&t, NULL, worker, NULL);
  assert(rc == 0);

  rc = sem_wait(&ready);
  assert(rc == 0);

  pid_t self_tid = (pid_t)syscall(SYS_gettid);

  assert(thread_blocks_signal(worker_tid, SIGUSR1) == 1);
  assert(thread_blocks_signal(self_tid, SIGUSR1) == 0);
  assert(thread_blocks_signal(999999 /* bogus tid, should not exist */, SIGUSR1) == 0);

  rc = pthread_cancel(t);
  assert(rc == 0);
  rc = pthread_join(t, NULL);
  assert(rc == 0);

  printf("PASS\n");
  return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
gcc -Wall -Werror -Iinclude src/lib/tsan_support.c test/tsan_support/test_thread_blocks_signal.c -o /tmp/test_thread_blocks_signal -lpthread
```

Expected: FAIL — `src/lib/tsan_support.c: No such file or directory` (it doesn't exist yet).

- [ ] **Step 3: Implement `thread_blocks_signal()`**

Create `src/lib/tsan_support.c`:

```c
#include "mcmini/spy/checkpointing/tsan_support.h"

#include <stdio.h>

int thread_blocks_signal(pid_t tid, int signo) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/task/%d/status", (int)tid);
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    return 0;
  }

  char line[256];
  unsigned long long sigblk = 0;
  int found = 0;
  while (fgets(line, sizeof(line), f)) {
    if (sscanf(line, "SigBlk: %llx", &sigblk) == 1) {
      found = 1;
      break;
    }
  }
  fclose(f);

  if (!found) {
    return 0;
  }
  return (int)((sigblk >> (signo - 1)) & 1ULL);
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```bash
gcc -Wall -Werror -Iinclude src/lib/tsan_support.c test/tsan_support/test_thread_blocks_signal.c -o /tmp/test_thread_blocks_signal -lpthread
/tmp/test_thread_blocks_signal
echo "exit: $?"
```

Expected: compiles with no warnings, prints `PASS`, `exit: 0`.

- [ ] **Step 5: Wire the new file into the CMake build**

Modify `CMakeLists.txt`, in the `LIBMCMINI_C_SRC` list (currently lines 68-77):

```cmake
  src/lib/dmtcp-callback.c
  src/lib/entry.c
  src/lib/interception.c
  src/lib/log.c
  src/lib/main.c
  src/lib/record.c
  src/lib/sem-wrappers.c
  src/lib/template/loop.c
  src/lib/template/sig.c
  src/lib/tsan_support.c
  src/lib/wrappers.c
```

(single line added: `  src/lib/tsan_support.c`, alphabetically before `src/lib/wrappers.c`)

Run:
```bash
cmake --build build --target libmcmini
```

Expected: `[100%] Built target libmcmini` with no warnings.

- [ ] **Step 6: Commit**

```bash
git add include/mcmini/spy/checkpointing/tsan_support.h src/lib/tsan_support.c CMakeLists.txt test/tsan_support/test_thread_blocks_signal.c
git commit -m "Add thread_blocks_signal() TSan-internal-thread probe"
```

---

### Task 2: Per-tid classification in `template_thread()`'s restart barrier

**Files:**
- Modify: `src/lib/dmtcp-callback.c:1-22` (add `#include` for the new header)
- Modify: `src/lib/dmtcp-callback.c:131-138` (add `get_tid_from_pthread_descriptor()` next to `patchThreadDescriptor()`)
- Modify: `src/lib/dmtcp-callback.c:368-384` (replace the blanket `thread_count -= 2` in `template_thread()` with per-tid classification)
- Test: `test/tsan_support/test_tid_from_descriptor_offset.c` (standalone host-side unit test, same style as Task 1)

**Interfaces:**
- Consumes: `int thread_blocks_signal(pid_t tid, int signo)` from Task 1 (`tsan_support.h`).
- Produces: `static inline pid_t get_tid_from_pthread_descriptor(pthread_t pthread_descriptor)` — file-local to `dmtcp-callback.c` (not exported; matches the file-local style of the neighboring `patchThreadDescriptor()`/`pthreadDescriptorTidOffset()`). No other task consumes it directly.

- [ ] **Step 1: Write a standalone test proving the read-only offset technique**

`get_tid_from_pthread_descriptor()` reads the same fixed offset into a `pthread_t` that the existing (mutating) `patchThreadDescriptor()` already uses and which is already self-verified at every restart (`saveThreadStateBeforeFork()`, `src/lib/dmtcp-callback.c:140-150`, aborts via `libc_abort()` on mismatch). The new risk this phase introduces is reading *another* thread's descriptor (the checkpoint thread's) instead of only ever reading `pthread_self()`. This test proves that part of the technique in isolation, independent of the rest of `dmtcp-callback.c`'s heavy dependencies (confirmed via `nm -u` on the compiled object file — over two dozen unresolved libmcmini-internal symbols — so linking the real file into a lightweight test binary is impractical; see the design spec for context).

Create `test/tsan_support/test_tid_from_descriptor_offset.c`:

```c
// Standalone host-side unit test. Not an mcmini model-checking target.
//
// Independently re-verifies the x86_64 pthread_t -> tid offset that
// src/lib/dmtcp-callback.c's pthreadDescriptorTidOffset() uses, specifically
// for READING (not patching) another thread's descriptor -- the new use case
// get_tid_from_pthread_descriptor() introduces. The existing
// patchThreadDescriptor() already self-verifies the same offset for
// `pthread_self()` on every restart (see saveThreadStateBeforeFork()); this
// test covers the "another thread's descriptor" case that path never
// exercises.
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef __x86_64__
#error "This standalone test only covers __x86_64__; see dmtcp-callback.c's pthreadDescriptorTidOffset() for other architectures."
#endif

static pid_t tid_from_descriptor(pthread_t descriptor) {
  const int offset = 720;  // matches pthreadDescriptorTidOffset() for __x86_64__
  return *(pid_t *)((char *)descriptor + offset);
}

static sem_t ready;
static pthread_t worker_self;
static pid_t worker_tid;

static void *worker(void *arg) {
  (void)arg;
  worker_self = pthread_self();
  worker_tid = (pid_t)syscall(SYS_gettid);
  sem_post(&ready);
  for (;;) {
    pause();
  }
  return NULL;
}

int main(void) {
  int rc = sem_init(&ready, 0, 0);
  assert(rc == 0);

  pthread_t t;
  rc = pthread_create(&t, NULL, worker, NULL);
  assert(rc == 0);

  rc = sem_wait(&ready);
  assert(rc == 0);

  // Read another thread's tid from its descriptor without mutating it.
  assert(tid_from_descriptor(worker_self) == worker_tid);
  // Reading twice must be idempotent (unlike patchThreadDescriptor()).
  assert(tid_from_descriptor(worker_self) == worker_tid);

  rc = pthread_cancel(t);
  assert(rc == 0);
  rc = pthread_join(t, NULL);
  assert(rc == 0);

  printf("PASS\n");
  return 0;
}
```

- [ ] **Step 2: Run the test to verify it passes**

Run:
```bash
gcc -Wall -Werror test/tsan_support/test_tid_from_descriptor_offset.c -o /tmp/test_tid_from_descriptor_offset -lpthread
/tmp/test_tid_from_descriptor_offset
echo "exit: $?"
```

Expected: compiles with no warnings, prints `PASS`, `exit: 0`. (This test is expected to pass immediately — it documents and locks in an already-proven assumption from elsewhere in the codebase, rather than driving new production code through a red/green cycle.)

- [ ] **Step 3: Include the new header in `dmtcp-callback.c`**

In `src/lib/dmtcp-callback.c`, current lines 21-22:

```c
#include "dmtcp.h"
#include "mcmini/mcmini.h"
```

Change to:

```c
#include "dmtcp.h"
#include "mcmini/mcmini.h"
#include "mcmini/spy/checkpointing/tsan_support.h"
```

- [ ] **Step 4: Add `get_tid_from_pthread_descriptor()`**

In `src/lib/dmtcp-callback.c`, current lines 131-138:

```c
static inline pid_t patchThreadDescriptor(pthread_t pthreadSelf) {
  int offset = pthreadDescriptorTidOffset();
  pid_t oldtid = *(pid_t *)((char *)pthreadSelf + offset);
  // Since glibc.2.25, tid, but not pid, is stored in pthread_t.
  // gettid() supported only in glibc-2.30; So, we use syscall().
  *(pid_t *)((char *)pthreadSelf + offset) = syscall(SYS_gettid);
  return oldtid;
}
```

Add immediately after it:

```c
// Read-only sibling of patchThreadDescriptor(): returns the tid recorded in
// `pthread_descriptor` without mutating it. Used to look up the checkpoint
// thread's tid (a *different* thread's descriptor) from the template thread;
// patchThreadDescriptor() is only ever called by a thread on its own
// descriptor.
static inline pid_t get_tid_from_pthread_descriptor(pthread_t pthread_descriptor) {
  int offset = pthreadDescriptorTidOffset();
  return *(pid_t *)((char *)pthread_descriptor + offset);
}
```

- [ ] **Step 5: Replace the blanket `-2` with per-tid classification in `template_thread()`**

In `src/lib/dmtcp-callback.c`, current lines 368-384:

```c
  int thread_count = 0;
  struct dirent *entry;
  DIR *dp = opendir("/proc/self/task");
  if (dp == NULL) {
    perror("opendir");
    mc_exit(EXIT_FAILURE);
  }

  while ((entry = readdir(dp)))
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
      thread_count++;

  // We don't want to count the template thread nor
  // the checkpoint thread, but these will appear in
  // `/proc/self/tasks`
  thread_count -= 2;
  closedir(dp);
```

Replace with:

```c
  int thread_count = 0;
  struct dirent *entry;
  DIR *dp = opendir("/proc/self/task");
  if (dp == NULL) {
    perror("opendir");
    mc_exit(EXIT_FAILURE);
  }

  const pid_t self_tid = syscall(SYS_gettid);
  const pid_t ckpt_tid = get_tid_from_pthread_descriptor(ckpt_pthread_descriptor);

  // Self-check: get_tid_from_pthread_descriptor() reads the same offset that
  // patchThreadDescriptor() already relies on and that saveThreadStateBeforeFork()
  // already self-verifies for `pthread_self()` on every restart. Confirm the
  // read-only variant agrees for the template thread's own descriptor before
  // trusting it to read the checkpoint thread's descriptor above.
  if (get_tid_from_pthread_descriptor(pthread_self()) != self_tid) {
    fprintf(stderr,
        "PID %d: template_thread(): get_tid_from_pthread_descriptor: "
        "bad offset:\n        Run: DMTCP:util/check-pthread-tid-offset.c\n",
        getpid());
    libc_abort();
  }

  // We don't want to count the template thread itself, the checkpoint
  // thread, or TSan-internal threads (e.g. libtsan's background thread,
  // which blocks all signals at creation and never calls into libmcmini's
  // wrappers, so it will never post to `dmtcp_restart_sem` below).
  while ((entry = readdir(dp))) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    pid_t tid = (pid_t)atoi(entry->d_name);
    if (tid == self_tid || tid == ckpt_tid) {
      continue;
    }
    if (thread_blocks_signal(tid, SIG_MULTITHREADED_FORK)) {
      log_debug("Excluding TSan-internal thread %d from the restart barrier\n", tid);
      continue;
    }
    thread_count++;
  }
  closedir(dp);
```

- [ ] **Step 6: Rebuild and verify**

Run:
```bash
cmake --build build --target libmcmini
```

Expected: `[100%] Built target libmcmini` with no warnings (build uses `-Wall -Werror`).

- [ ] **Step 7: Commit**

```bash
git add src/lib/dmtcp-callback.c test/tsan_support/test_tid_from_descriptor_offset.c
git commit -m "Exclude TSan-internal threads from template_thread()'s restart barrier"
```

---

### Task 3: End-to-end verification against a real TSan target (environment-gated)

**Precondition:** this task requires the DMTCP toolchain (`dmtcp_launch`, `dmtcp_restart`) installed and on `PATH`, and the project built with `MCMINI_WITH_DMTCP=ON`. Neither is currently true in this dev environment (`which dmtcp_launch` returns nothing; `CMakeLists.txt:20` has `set(MCMINI_WITH_DMTCP OFF)`). If this environment is unavailable when this task is reached, stop after Task 2, report that Tasks 1-2 are complete and committed, and hand this task off to whoever has a DMTCP-enabled environment — do not mark this task done without actually running it.

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

From the directory containing `libmcmini.so` (per the `mcmini` cwd-relative plugin-path gotcha documented in `CLAUDE.md`):

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

Expected (this phase's pass criterion, per `PLAN.txt` Phase 1 and the design spec's Testing section): the `template_thread()` debug log (enable with `MCMINI_LOG_LEVEL=debug` if not already visible) shows a thread count matching the real number of application threads, any `Excluding TSan-internal thread <tid> from the restart barrier` lines for libtsan's background thread, and `The threads are now in a consistent state` — **not** an indefinite hang. Reaching (or failing to reach) the model-checker handshake afterward is Phase 2's concern (R2/R3/R4), not this task's pass/fail criterion.

- [ ] **Step 5: Record the observed outcome**

No commit for this task (no source changes). If the pass criterion in Step 4 is met, note it in the PR/handoff description; if not, capture the log output and hand off to Phase 2 planning rather than attempting further fixes here (out of scope for R1).
