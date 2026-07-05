# Design: TSan Port Phase 3 (R5) — pthread_join / pthread_exit shim for recreated threads

Date: 2026-07-05
Related: `PLAN.txt` (Phase 3), branch `tsan-record-thread-fix`
(current tip after Phase 0-2: commit `1b2354a`).

## Problem

`PLAN.txt` Phase 3 calls for porting R5 from the standalone package
`multithreaded-fork-tsan-2.0`: a `pthread_join`/`pthread_exit` shim so that
threads recreated via `restart_child_threads_fast()`'s `__clone()` (which are
TSan fibers, not TSan-registered "real" threads) can be joined and can exit
without tripping ThreadSanitizer's interceptors. PLAN.txt itself flags this as
"LIKELY SIMPLER than the standalone" and lists two open questions (§8, Q1)
that this design resolves via direct code-path analysis of the current
codebase (DMTCP is unavailable in this environment, so this analysis is
static, not a runtime reproduction — see Testing below).

## Investigation findings

### `pthread_join`: no code change needed

`mc_pthread_join` (`src/lib/wrappers.c:732-810`) already strongly overrides
the public `pthread_join` symbol (via `interception.c:226-228`) in every
`libmcmini_mode`. Reading its full mode-dispatch:

- `PRE_CHECKPOINT_THREAD` / `CHECKPOINT_THREAD`: forwards to
  `libdmtcp_pthread_join` (a real, DMTCP-provided join).
- `RECORD` / `PRE_CHECKPOINT`: uses `libpthread_timedjoin_np` — already a
  libtsan-bypassing handle, per the existing comment at
  `interception.h:32-35`, for an unrelated, already-solved DMTCP/TSan
  interaction (see `TSAN-McMini-DMTCP.txt`).
- `DMTCP_RESTART_INTO_BRANCH` / `DMTCP_RESTART_INTO_TEMPLATE` /
  `TARGET_BRANCH` / `TARGET_BRANCH_AFTER_RESTART`: writes `THREAD_JOIN_TYPE`
  into the shared-memory mailbox and calls `thread_wake_scheduler_and_wait()`
  (or `thread_handle_after_dmtcp_restart()` pre-restart) — **a pure
  mailbox/semaphore handshake with the `mcmini` scheduler process. No real
  libc/libpthread join call is made on any of these paths.**

Since libtsan's `pthread_join` interceptor can only fire if a call actually
reaches a real `pthread_join`/`pthread_timedjoin_np` symbol, and no code path
that applies to a recreated thread (which only exists in `TARGET_BRANCH*`
modes, post-restart) ever makes such a call, **libtsan's join interceptor
cannot fire for a recreated thread's join in this codebase.** This resolves
PLAN.txt §8 Q1: the standalone's join `CHECK failed` does not reproduce here,
because the crash-prone code path is structurally unreachable.

### `pthread_exit`: real gap, but simpler shim than the standalone's

`pthread_exit` is not intercepted anywhere in this codebase today (confirmed
via exhaustive grep — no `mc_pthread_exit`, no override in `interception.c`).
Two consequences:

1. **TSan-safety gap** (PLAN.txt's stated concern): a recreated (fiber-only)
   thread calling `pthread_exit()` explicitly falls through to libtsan's real
   interceptor, which asserts the caller is a genuine TSan thread — a likely
   crash, matching the standalone's documented failure.
2. **Pre-existing model-checking gap** (found during this investigation, not
   in PLAN.txt): McMini's model already has full `THREAD_EXIT_TYPE`
   machinery — `mc_exit_thread_in_child()` / `mc_exit_main_thread_in_child()`
   (`wrappers.c:349-375`), wired into the transition registry
   (`include/mcmini/model/transitions/thread/thread_exit.hpp`,
   `src/mcmini/model/transition_registry.cpp:18`) — but it is only invoked
   today when a thread's routine *returns normally*, via
   `mc_thread_routine_wrapper()`'s epilogue (`wrappers.c:533-561`). An
   explicit `pthread_exit()` call bypasses this entirely, for *any* thread,
   TSan or not, today.

The user chose to fix both together (see Approach below).

**Why this shim can be simpler than the standalone's stash+fiber+raw-exit
design:** reading `mc_exit_thread_in_child()`/`mc_exit_main_thread_in_child()`
in full — both end in `thread_block_indefinitely()` (`wrappers.c:122-126`,
`while(1) pause();`). **Neither ever terminates the OS thread at the kernel
level.** McMini's "thread exit" is purely a model/scheduler bookkeeping
event; the whole branch *process* is discarded after its trace is explored,
not individual threads. This means:

- There is no OS-level termination this shim needs to arrange (the
  standalone's `CLONE_CHILD_CLEARTID`-triggered real-join-wakeup has no
  analog here — Finding 1 above already established joins never touch real
  OS join primitives in these modes).
- A recreated thread already has a valid TSan fiber by the time any of its
  own code runs post-resume (the R4 fix, Phase 2, switches it in
  immediately). Calling `mc_exit_thread_in_child()` — itself just ordinary
  mutex/semaphore/shared-memory calls — is no more dangerous than any other
  TSan-instrumented call that thread already makes routinely (e.g.
  `thread_await_scheduler()`, called immediately after every resume). No
  fiber switch is needed *inside* the exit shim specifically.
- `mc_pthread_join`'s `TARGET_BRANCH*` case (already read in full above)
  never touches its `void **rv` output parameter — a joined thread's return
  value is already unconditionally discarded in this model-checked path
  today. A retval-stash mechanism (the standalone's
  `mtf_set_exit_retval`/`mtf_get_exit_retval`) would stash a value nothing
  downstream ever reads.

Net effect: **no `mtf_is_recreated_thread()`, no `exit_retval` field, no
fiber switch inside the shim** — a real simplification vs. the standalone
package, matching PLAN.txt's own "likely simpler" prediction.

## Design

### `mc_pthread_exit()` — new interceptor in `src/lib/wrappers.c`

Placed alongside `mc_transparent_exit()`/`mc_transparent_abort()`
(`wrappers.c:377-421`), whose mode-dispatch shape it mirrors:

```c
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
```

(`libpthread_pthread_exit()` and `mc_exit_main_thread_in_child()` /
`mc_exit_thread_in_child()` are all `MCMINI_NO_RETURN`-equivalent in
practice — they either call a real `noreturn` function or park forever —
so no `return`/`break` is reachable in any branch, matching the function's
own `MCMINI_NO_RETURN` contract.)

**Mode coverage, mirroring `mc_pthread_join`'s exact enumeration style**
(`enum libmcmini_mode` has 11 values, `record.h:124-139`):

- Pre-restart modes (real OS threads, not yet under model-checker control):
  forward to the real `pthread_exit`, preserving `retval`. Unlike
  `mc_pthread_join`, `PRE_DMTCP_INIT` does not need a special `assert(0)`
  here — that case in `mc_pthread_join` guards a DMTCP-internal-join edge
  case with no analog for a target's own `pthread_exit()` call.
- Restart/branch modes: dispatch on the *existing* `tid_self` TLS variable
  (`wrappers.c:57`, already set by `mc_register_this_thread()` — no new
  state needed) to pick the main-thread-preserving variant vs. the normal
  one, exactly mirroring `mc_thread_routine_wrapper()`'s own implicit
  choice (it only ever calls `mc_exit_thread_in_child()`, since `main()`
  never runs through that wrapper in the first place).
- `default:` (covers `TARGET_TEMPLATE` / `TARGET_TEMPLATE_AFTER_RESTART`,
  the two `enum` values `mc_pthread_join` also doesn't enumerate) →
  `libc_abort()`, consistent with the existing invariant that a template
  process's userspace threads are permanently parked and should never reach
  wrapper code in these modes.

### `libpthread_pthread_exit()` — new cached real-function handle

Follows the established `dlopen`+`dlsym` pattern used by every existing
`libpthread_*`/`libdmtcp_*` handle (`interception.c:15-40` declarations,
`:46-112` resolution in `mc_load_intercepted_pthread_functions()`,
`:114-300`ish forwarding wrappers) — a single new handle, no
`libdmtcp_pthread_exit` variant (unlike join, there is no documented
DMTCP-specific interception concern for `pthread_exit`, so one handle
covers every pre-restart mode uniformly):

```c
// interception.c, near the other pointer declarations (~line 18):
__attribute__((__noreturn__)) typeof(&pthread_exit) libpthread_pthread_exit_ptr;

// in mc_load_intercepted_pthread_functions(), near the other libpthread_handle
// resolutions (~line 73):
libpthread_pthread_exit_ptr = dlsym(libpthread_handle, "pthread_exit");

// new forwarding wrappers, alongside pthread_join's family (~line 241):
MCMINI_NO_RETURN void pthread_exit(void *retval) {
  mc_pthread_exit(retval);
}
MCMINI_NO_RETURN void libpthread_pthread_exit(void *retval) {
  libmcmini_init();
  (*libpthread_pthread_exit_ptr)(retval);
}
```

This handle is TSan-bypassing by construction (same `dlopen`'d-fresh-module
technique as every existing handle in this family), matching this
codebase's uniform existing convention (`libc_exit`, `libc_abort`,
`libpthread_timedjoin_np`, `libdmtcp_pthread_join` are all TSan-bypassing
the same way) rather than a new, inconsistent choice for this one call.

### Declarations

- `include/mcmini/spy/intercept/wrappers.h` (near `mc_pthread_join` at `:28`
  and `mc_transparent_exit`/`abort` at `:47-48`):
  `MCMINI_NO_RETURN void mc_pthread_exit(void *retval);`
- `include/mcmini/spy/intercept/interception.h` (near the `pthread_join`
  family at `:29-35`): `void pthread_exit(void *) __attribute__((__noreturn__));`
  and `void libpthread_pthread_exit(void *) __attribute__((__noreturn__));`

## Error handling & edge cases

- `default:` case (`TARGET_TEMPLATE` / `TARGET_TEMPLATE_AFTER_RESTART`)
  aborts, matching the existing invariant already enforced the same way by
  `mc_pthread_join`.
- The main-thread-vs-not dispatch reuses `tid_self` (already-existing TLS
  state, `wrappers.c:57`) — no new per-thread state is introduced.
- No new `struct threadinfo` fields, no new global tables.

## Testing

1. **`pthread_join`**: no code change, so no new test — the "no fix needed"
   conclusion rests on the static mode-dispatch analysis above (per user
   decision, sufficient given there is no dynamic behavior on the mailbox
   path to exercise, and DMTCP is unavailable here for a live reproduction).
2. **`libpthread_pthread_exit()` (pre-restart forwarding half)**: a
   standalone test proving the new cached handle resolves and correctly
   terminates a thread with the given `retval`, verified via a real
   `pthread_join()` on that thread returning the expected value. This
   exercises the same `RECORD`/pre-restart code path in isolation, without
   needing DMTCP.
3. **Restart-mode dispatch (`mc_exit_thread_in_child`/
   `mc_exit_main_thread_in_child` routing)**: tightly coupled to the real
   shared-memory mailbox protocol and the live `mcmini` scheduler process —
   unlike Phase 2's fork/clone/fiber mechanism, not practically mirrorable
   in a self-contained standalone harness without reimplementing a chunk of
   the scheduler's own protocol. This becomes an environment-gated
   end-to-end task, matching Phase 1/2's Task 3 pattern (requires the
   DMTCP toolchain, absent in this environment).
4. **Production build**: `cmake --build build --target libmcmini` must stay
   clean under `-Wall -Werror`.

## Out of scope for this phase

- Any change to `mc_pthread_join` (confirmed unnecessary).
- `mtf_is_recreated_thread()`, `exit_retval` field, or any fiber switch
  inside the exit shim (confirmed unnecessary by the "never really
  terminates the OS thread" finding above).
- Phase 4 (PLAN.txt: hardening — multiple sequential branches, higher
  thread counts, real example targets built with `-fsanitize=thread`).
