# Design: TSan Port Phase 2 (R2 + R3 + R4) — fork hooks, __clone, fiber switching

Date: 2026-07-04
Related: `PLAN.txt` (Phase 2), branch `tsan-record-thread-fix`
(current tip after Phase 0 + Phase 1: commit `74d5126`).

## Problem

`PLAN.txt` Phase 2 calls for three of the five fixes proven in the standalone
package `multithreaded-fork-tsan-2.0` to be ported into libmcmini's fast
restart path (`src/lib/dmtcp-callback.c`):

- **R2** — rerun TSan's fork pipeline around the raw `_Fork()` call in
  `fast_multithreaded_fork()`.
- **R3** — recreate threads with libc's `__clone`, not the public `clone()`
  (libtsan intercepts `clone()` and treats every call as a fork, corrupting
  its thread-slot state for a `CLONE_THREAD` clone).
- **R4** — give every recreated thread, and the forking thread itself, a
  fresh TSan fiber (a TSan `ThreadState` decoupled from the OS thread) before
  either makes a TSan-intercepted call.

Inspection of the current `dmtcp-callback.c` (inherited from Phase 0's
"keeper" prototypes, `PLAN.txt` section 5) shows **R2 and half of R4 are
already committed**:

- R2 is fully done: `fast_multithreaded_fork()` already brackets `_Fork()`
  with `__sanitizer_syscall_pre_impl_fork()` / `__sanitizer_syscall_post_impl_fork()`
  (`dmtcp-callback.c:236-242`).
- R4's per-*recreated*-thread half is done: `thread_handle_after_dmtcp_restart()`
  already switches onto a fresh fiber at its post-`setcontext()` resume point
  (`dmtcp-callback.c:304-315`), satisfying PLAN.txt's "pick one [placement],
  not both" instruction (the standalone's alternative placement, inside
  `child_setcontext_fast()` before `setcontext()`, was not used and should
  not be added — that would be the "both" PLAN.txt warns against).

What remains:

- **R3** is not done: `restart_child_threads_fast()` (`:186-208`) still calls
  the public `clone()` (`:203-206`).
- **R4's other half** is not done: the *forking thread* (the thread that
  calls `fast_multithreaded_fork()`, running on in the child after `_Fork()`)
  never gets a fresh fiber. The standalone's `README` documents why this is
  a separate, required fix: the forking thread keeps its fork-inherited TSan
  `ThreadState`, whose shadow call stack starts at the parent's fork-time
  depth and can overflow (crash in TSan's `FuncEntry`) as that thread keeps
  running.

## Design

### Production code changes (`src/lib/dmtcp-callback.c`)

**R3 — `__clone` instead of `clone()`:**

Add a non-weak extern declaration next to the existing weak TSan externs
(`:37-51`):

```c
extern int __clone(int (*fn)(void *), void *child_stack, int flags,
                    void *arg, ... /* pid_t *ptid, void *newtls, pid_t *ctid */);
```

(`__clone` is a normal libc-internal symbol, always present — unlike the
TSan hooks, which are weak because they only resolve when the binary links
libtsan.)

In `restart_child_threads_fast()` (`:186-208`), replace the call to
`clone(child_setcontext_fast, stack, clone_flags, (void *)&threadInfos[i], ptid, threadInfos[i].fs, ctid)`
(`:203-206`) with the same call to `__clone(...)`. No other logic in that
function changes.

**R4 remainder — fresh fiber for the forking thread:**

In `fast_multithreaded_fork()`'s child branch (`:273-275`, currently just
`if (childpid == 0) { restart_child_threads_fast(); }`), add the fiber
switch *before* calling `restart_child_threads_fast()`:

```c
if (childpid == 0) { // child process
  // The forking thread keeps its inherited (fork-copied) TSan ThreadState,
  // whose shadow call stack starts at the parent's fork-time depth and can
  // overflow as this thread keeps running. Switch it onto a fresh fiber too.
  if (__tsan_switch_to_fiber != NULL) {
    __tsan_switch_to_fiber(__tsan_create_fiber(0), 0);
  }
  restart_child_threads_fast();
}
```

This mirrors the standalone's exact placement and rationale
(`multithreaded_fork.c:450-457` in the reference package).

### Standalone test harness

The vendor package's own tests (`mtf_park`, `mtf_join`, `mtf_exit`) use a
realtime-signal broadcast to snapshot each thread's context — a materially
different mechanism from the fast path, which has no signal barrier at all
(per PLAN.txt D1): each thread calls `getcontext()` directly, as a plain
function call, when it happens to pass through
`thread_handle_after_dmtcp_restart()`. Reusing the vendor tests as-is would
validate the *fixes* but not the *actual resumption mechanism* this port
targets.

New file: `test/tsan_support/test_fastpath_fork_clone_fiber.c` — a standalone
harness (compiled/run directly with `gcc -fsanitize=thread`, not via CMake,
matching Phase 1's `test/tsan_support/` convention) that mirrors
`dmtcp-callback.c`'s actual mechanism instead of the vendor's signal-based
one:

- N worker threads (3, matching the vendor's default) each call `getcontext()`
  directly inside their thread body (no signal handler), record their
  `pthread_t`/TLS pointer, and — on the first pass (`getpid() == orig_pid`)
  — post to a check-in semaphore and block. The main thread waits for all N
  check-ins, then calls a `fast_multithreaded_fork()`-equivalent.
- That equivalent function reproduces R2 (fork hooks around `_Fork()`), the
  new R4-remainder fix (fiber switch for the forking thread), and calls a
  `restart_child_threads_fast()`-equivalent that uses `__clone()` (R3) to
  recreate each worker via `child_setcontext_fast()`-equivalent
  (`setTLSPointer` + `patchThreadDescriptor` + `setcontext`).
- Each recreated thread resumes exactly at its `getcontext()` call site and,
  on this second pass (`getpid() != orig_pid`), takes the already-proven
  fiber-switch branch (mirroring `thread_handle_after_dmtcp_restart()`'s
  existing code) before proceeding.
- All workers (original, in the parent; recreated, in the child) then run
  the same TSan-intercepted stress workload as the vendor's `test_park.c`: a
  mutex-protected shared-counter increment loop, 100,000 iterations each.
  Pass criterion: both parent and child print
  `shared_counter=300000 (expected 300000)`, with no
  `ThreadSanitizer: CHECK failed`, SEGV, or hang.
- Recreated threads never `pthread_join`/`pthread_exit` — they park forever
  and the process exits via `_exit()`, matching the vendor's `test_park.c`
  (R5 join/exit shims are PLAN.txt Phase 3, out of scope here).

The harness necessarily duplicates a handful of small helpers already in
`dmtcp-callback.c` (`getTLSPointer`/`setTLSPointer`/`patchThreadDescriptor`/
`pthreadDescriptorTidOffset`, x86_64-only) — the same accepted-duplication
precedent as Phase 1's `test_tid_from_descriptor_offset.c`, since linking
the real `dmtcp-callback.c` is impractical (confirmed in Phase 1 via `nm -u`:
~30 unresolved libmcmini-internal symbols).

Run via `setarch -R` (disables ASLR) with
`TSAN_OPTIONS="handle_segv=0 die_after_fork=0"` — confirmed during this
design's investigation to make ThreadSanitizer work in this sandbox (a
bare `-fsanitize=thread` binary fails immediately with
`FATAL: ThreadSanitizer: unexpected memory mapping` without `setarch -R`;
with it, the vendor package's own `make check` passes all three of its
demos here).

## Error handling & edge cases

- `__clone`'s temporary per-thread stack (`malloc`'d in
  `restart_child_threads_fast()`) is intentionally leaked, matching the
  existing `// FIXME: This stack is a memory leak` comment at `:196-197` —
  not a regression introduced or fixed by this phase, and irrelevant for a
  short-lived branch/test process.
- The harness's check-in barrier is a plain counting semaphore with no
  TSan-thread-exclusion logic (R1) — the harness only ever spawns its own
  known worker threads, so there is no libtsan background thread to filter,
  and R1 is already solved (Phase 1) and orthogonal to R2/R3/R4.

## Testing / verification

1. **Standalone harness**: built and run under `setarch -R` with the
   `TSAN_OPTIONS` above; pass criterion is both parent and child printing
   `shared_counter=300000 (expected 300000)` with no CHECK-failure/SEGV/hang.
2. **Production build**: `cmake --build build --target libmcmini` after the
   R3/R4-remainder changes, must stay clean under `-Wall -Werror`.
3. **Environment-gated end-to-end task** (same posture as Phase 1's Task 3):
   PLAN.txt's actual Phase 2 pass criterion — a real branch child reaching
   the model-checker handshake under DMTCP + `--multithreaded-fork` without
   SIGSEGV/CHECK — still requires the DMTCP toolchain, which remains absent
   in this environment. Documented with exact commands for whoever has that
   environment; not executed here.

## Out of scope for this phase

- R1 (already done, Phase 1) and R5 (join/exit shims — PLAN.txt Phase 3).
- The model-checker handshake (`thread_await_scheduler()`) and any
  DMTCP-dependent verification.
- Re-touching the already-committed R2 fork-hook bracketing or the
  recreated-thread fiber switch (both already correct and reviewed in
  Phase 0/1; only the two gaps identified above are in scope).
