# TSan Port Phase 3 (R5): pthread_join / pthread_exit shim — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `pthread_exit()` interceptor (`mc_pthread_exit`) that fixes both a TSan-safety gap (a recreated/fiber thread's explicit `pthread_exit()` call currently falls through to libtsan's real interceptor, which can crash) and a pre-existing model-checking gap (explicit `pthread_exit()` calls are invisible to McMini's scheduler today, for every thread). `pthread_join` needs no change — confirmed unnecessary by static analysis in the design spec.

**Architecture:** One new interceptor function, `mc_pthread_exit()`, mirroring the existing `mc_transparent_exit()`/`mc_transparent_abort()` mode-dispatch shape: pre-restart modes forward to a newly-added cached "real pthread_exit" handle (matching this codebase's established `dlopen`+`dlsym` bypass pattern); restart/branch modes route into the *already-existing* `mc_exit_thread_in_child()`/`mc_exit_main_thread_in_child()` machinery, for every thread uniformly (no per-thread "is this recreated" tracking needed — see design spec for why).

**Tech Stack:** C11, CMake, `dlopen`/`dlsym`, POSIX threads.

## Global Constraints

- Build uses `-Wall -Werror` (`CMakeLists.txt:81`) — every change must compile warning-free.
- Design reference: `docs/superpowers/specs/2026-07-05-tsan-port-phase3-r5-design.md`. Follow it for rationale (in particular: why no `mtf_is_recreated_thread()`, no `exit_retval` field, and no fiber switch inside the shim are needed here, unlike the standalone package).
- Baseline: branch `tsan-record-thread-fix`, commit `e6575df` (Phase 3 design spec, on top of prior phases).
- `pthread_join` requires **no code change** — this plan does not touch `mc_pthread_join` at all.
- This project has no C unit-test harness (per `CLAUDE.md`); standalone tests are compiled and run directly with `gcc`, never via CMake.

---

### Task 1: Standalone test proving the `libpthread_pthread_exit` forwarding technique

**Files:**
- Create: `test/tsan_support/test_pthread_exit_forwarding.c`

**Interfaces:** None consumed from other tasks. Produces no interface other tasks depend on — a standalone, self-contained proof that `dlopen("libpthread.so"/"libpthread.so.0")` + `dlsym(..., "pthread_exit")` resolves to a working real `pthread_exit` that correctly terminates a thread and delivers its `retval` to a real `pthread_join()`. Task 2 applies this exact technique to production, following the identical `dlopen`+`dlsym` pattern already used by every other `libpthread_*` handle in `src/lib/interception.c`.

- [ ] **Step 1: Write the test**

Create `test/tsan_support/test_pthread_exit_forwarding.c`:

```c
// Standalone host-side unit test. Not an mcmini model-checking target.
//
// Proves the dlopen+dlsym technique Task 2 uses to add a cached "real
// pthread_exit" handle to src/lib/interception.c (matching the existing
// libpthread_pthread_join_ptr/libpthread_timedjoin_np_ptr pattern): resolve
// pthread_exit from a freshly dlopen'd libpthread.so/libpthread.so.0, call
// it from a thread, and confirm the retval reaches a real pthread_join().
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>

typedef void (*pthread_exit_fn)(void *);

static pthread_exit_fn real_pthread_exit;

static void *worker(void *arg) {
  (void)arg;
  real_pthread_exit((void *)(long)42);
  return NULL; // not reached
}

int main(void) {
  void *libpthread_handle = dlopen("libpthread.so", RTLD_LAZY);
  if (!libpthread_handle) {
    libpthread_handle = dlopen("libpthread.so.0", RTLD_LAZY);
  }
  assert(libpthread_handle != NULL);

  real_pthread_exit = (pthread_exit_fn)dlsym(libpthread_handle, "pthread_exit");
  assert(real_pthread_exit != NULL);

  pthread_t t;
  int rc = pthread_create(&t, NULL, worker, NULL);
  assert(rc == 0);

  void *retval = NULL;
  rc = pthread_join(t, &retval);
  assert(rc == 0);
  assert((long)retval == 42);

  printf("PASS\n");
  return 0;
}
```

- [ ] **Step 2: Compile and run — verify it passes**

Run:
```bash
gcc -Wall -Werror test/tsan_support/test_pthread_exit_forwarding.c -o /tmp/test_pthread_exit_forwarding -ldl -lpthread
/tmp/test_pthread_exit_forwarding
echo "exit: $?"
```

Expected: compiles clean, prints `PASS`, `exit: 0`. (This test is expected to pass immediately — like Phase 1's `test_tid_from_descriptor_offset.c`, it documents and locks in a technique this codebase already uses elsewhere for other symbols, rather than driving new production code through a red/green cycle.)

- [ ] **Step 3: Commit**

```bash
git add test/tsan_support/test_pthread_exit_forwarding.c
git commit -m "Add standalone test proving the libpthread_pthread_exit dlopen+dlsym technique"
```

---

### Task 2: Add `mc_pthread_exit()` interceptor to production

**Files:**
- Modify: `src/lib/interception.c:19` (add `libpthread_pthread_exit_ptr` declaration)
- Modify: `src/lib/interception.c:73` (resolve it in `mc_load_intercepted_pthread_functions()`)
- Modify: `src/lib/interception.c:241-242` (add the `pthread_exit`/`libpthread_pthread_exit` forwarding functions)
- Modify: `include/mcmini/spy/intercept/interception.h:35` (declare both)
- Modify: `include/mcmini/spy/intercept/wrappers.h:48` (declare `mc_pthread_exit`)
- Modify: `src/lib/wrappers.c:466` (add the `mc_pthread_exit()` implementation)

**Interfaces:**
- Consumes: nothing from Task 1 directly (Task 1 is a standalone proof of technique, not linked code) — but Task 2 must produce a byte-for-byte-equivalent `dlopen`+`dlsym` resolution to what Task 1 proved works.
- Produces: `MCMINI_NO_RETURN void mc_pthread_exit(void *retval);` (declared in `wrappers.h`) and the public `void pthread_exit(void *) __attribute__((__noreturn__));` override (declared in `interception.h`). No later task in this plan consumes these directly; Task 3 exercises them end-to-end.

- [ ] **Step 1: Declare the new cached-handle pointer**

In `src/lib/interception.c`, current line 19:

```c
typeof(&pthread_timedjoin_np) libpthread_timedjoin_np_ptr;
```

Add immediately after:

```c
__attribute__((__noreturn__)) typeof(&pthread_exit) libpthread_pthread_exit_ptr;
```

- [ ] **Step 2: Resolve it in `mc_load_intercepted_pthread_functions()`**

In `src/lib/interception.c`, current line 73:

```c
  libpthread_timedjoin_np_ptr = dlsym(libpthread_handle, "pthread_timedjoin_np");
```

Add immediately after:

```c
  libpthread_pthread_exit_ptr = dlsym(libpthread_handle, "pthread_exit");
```

- [ ] **Step 3: Add the public override and the forwarding wrapper**

In `src/lib/interception.c`, current lines 238-242:

```c
int libdmtcp_pthread_join(pthread_t thread, void **rv) {
  libmcmini_init();
  return (*libdmtcp_pthread_join_ptr)(thread, rv);
}

void exit(int status) {
```

Replace with:

```c
int libdmtcp_pthread_join(pthread_t thread, void **rv) {
  libmcmini_init();
  return (*libdmtcp_pthread_join_ptr)(thread, rv);
}

MCMINI_NO_RETURN void pthread_exit(void *retval) {
  mc_pthread_exit(retval);
}
MCMINI_NO_RETURN void libpthread_pthread_exit(void *retval) {
  libmcmini_init();
  (*libpthread_pthread_exit_ptr)(retval);
}

void exit(int status) {
```

- [ ] **Step 4: Declare the new interception.c functions in the header**

In `include/mcmini/spy/intercept/interception.h`, current line 35:

```c
int libpthread_timedjoin_np(pthread_t thread, void**, const struct timespec*);
```

Add immediately after (blank line, then the two declarations):

```c

void pthread_exit(void *) __attribute__((__noreturn__));
// TSan-safe (libtsan-bypassing) handle for pthread_exit, used by
// mc_pthread_exit's pre-restart forwarding path.
void libpthread_pthread_exit(void *) __attribute__((__noreturn__));
```

- [ ] **Step 5: Declare `mc_pthread_exit` in wrappers.h**

In `include/mcmini/spy/intercept/wrappers.h`, current line 48:

```c
MCMINI_NO_RETURN void mc_transparent_exit(int status);
```

Add immediately after:

```c
MCMINI_NO_RETURN void mc_pthread_exit(void *retval);
```

- [ ] **Step 6: Implement `mc_pthread_exit()` in wrappers.c**

In `src/lib/wrappers.c`, find the end of `mc_transparent_abort()` — current lines 462-468:

```c
    default: {
      libc_abort();
    }
  }
}

struct mc_thread_routine_arg {
```

Replace with:

```c
    default: {
      libc_abort();
    }
  }
}

MCMINI_NO_RETURN void mc_pthread_exit(void *retval) {
  switch (get_current_mode()) {
    case PRE_DMTCP_INIT:
    case PRE_CHECKPOINT_THREAD:
    case CHECKPOINT_THREAD:
    case RECORD:
    case PRE_CHECKPOINT: {
      libpthread_pthread_exit(retval);
    }
    case DMTCP_RESTART_INTO_BRANCH:
    case DMTCP_RESTART_INTO_TEMPLATE:
    case TARGET_BRANCH:
    case TARGET_BRANCH_AFTER_RESTART: {
      if (tid_self == RID_MAIN_THREAD) {
        mc_exit_main_thread_in_child();
      } else {
        mc_exit_thread_in_child();
      }
    }
    default: {
      libc_abort();
    }
  }
}

struct mc_thread_routine_arg {
```

(This exact text is unique in the file: `mc_transparent_exit()`'s `default:` case calls `libc_exit(status)`, not `libc_abort()`, so only `mc_transparent_abort()`'s closing block is immediately followed by the blank line and `struct mc_thread_routine_arg {` declaration. Already verified by test-applying this exact replacement in a scratch copy before this plan was written.)

- [ ] **Step 7: Rebuild and verify**

Run:
```bash
cmake --build build --target libmcmini
```

Expected: `[100%] Built target libmcmini` with no warnings (build uses `-Wall -Werror`).

- [ ] **Step 8: Commit**

```bash
git add src/lib/interception.c src/lib/wrappers.c include/mcmini/spy/intercept/interception.h include/mcmini/spy/intercept/wrappers.h
git commit -m "Add mc_pthread_exit() interceptor (R5: pthread_exit shim)"
```

---

### Task 3: End-to-end verification against a real TSan target under DMTCP (environment-gated)

**Precondition:** this task requires the DMTCP toolchain (`dmtcp_launch`, `dmtcp_restart`) installed and on `PATH`, and the project built with `MCMINI_WITH_DMTCP=ON`. As of this plan's writing, this environment has neither (same gap Phase 1 and Phase 2's Task 3 hit). If unavailable when this task is reached, stop after Task 2, report Tasks 1-2 complete and committed, and hand this task off — do not mark it done without actually running it.

**Files:** none (this task runs commands and observes output; it does not change source).

- [ ] **Step 1: Reconfigure and rebuild with DMTCP enabled**

```bash
cmake -S . -B build -DMCMINI_WITH_DMTCP=ON
cmake --build build --target libmcmini
```

Expected: build succeeds and produces `build/libmcmini.so`.

- [ ] **Step 2: Build a TSan-instrumented example target that calls pthread_exit() explicitly**

The existing example targets under `src/examples/` may not call `pthread_exit()` explicitly (most likely just `return` from their thread routines). Write a minimal target that does, so this task actually exercises the new interceptor:

```c
// /tmp/pthread_exit_target.c -- minimal target exercising explicit pthread_exit()
#include <pthread.h>
#include <stdio.h>

static void *worker(void *arg) {
  (void)arg;
  printf("worker exiting via pthread_exit()\n");
  pthread_exit((void *)1);
  return NULL; // not reached
}

int main(void) {
  pthread_t t;
  pthread_create(&t, NULL, worker, NULL);
  void *retval;
  pthread_join(t, &retval);
  printf("joined, retval=%ld\n", (long)retval);
  return 0;
}
```

```bash
gcc -fsanitize=thread -g -pthread -o /tmp/pthread_exit_target-tsan /tmp/pthread_exit_target.c
```

Expected: compiles with no errors.

- [ ] **Step 3: Record a checkpoint**

From the directory containing `libmcmini.so`:

```bash
cd build
TSAN_OPTIONS="handle_segv=0 die_after_fork=0" \
  dmtcp_launch --disable-alloc-plugin -i 5 --with-plugin "$PWD/libmcmini.so" \
  /tmp/pthread_exit_target-tsan
```

Let it run for at least one checkpoint interval (5s) — the target itself finishes almost instantly, so this may need a `sleep` added to `worker()` before `pthread_exit()` to give the checkpoint interval time to fire; adjust the target in Step 2 if the process exits before a checkpoint is taken. Stop it (Ctrl-C) once a `ckpt_*.dmtcp` file appears in the current directory.

Expected: a `ckpt_pthread_exit_target-tsan_*.dmtcp` file is created.

- [ ] **Step 4: Restart under `mcmini` with `--multithreaded-fork`**

```bash
setarch -R ./mcmini --from-checkpoint ckpt_pthread_exit_target-tsan_*.dmtcp --multithreaded-fork /tmp/pthread_exit_target-tsan
```

Expected (this phase's pass criterion): the branch reaches and executes the target's `pthread_exit()` call without SIGSEGV or `ThreadSanitizer: CHECK failed`, and the join in `main()` completes. This is the R5 analog of Phase 2's Task 3 pass criterion (reaching the model-checker handshake without crashing), extended to cover an explicit `pthread_exit()` call specifically.

- [ ] **Step 5: Record the observed outcome**

No commit for this task (no source changes, and the scratch target file lives outside the repo in `/tmp`). If the pass criterion in Step 4 is met, note it in the PR/handoff description; if not, capture the log output and hand off to Phase 4 planning rather than attempting further fixes here.
