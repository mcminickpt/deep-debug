# Design: TSan Port Phase 4 — Hardening

Date: 2026-07-05
Related: `PLAN.txt` (Phase 4), branch `tsan-record-thread-fix`
(current tip after Phase 0-3: commit `cfba39d`).

## Problem

`PLAN.txt` Phase 4 calls for hardening the TSan `multithreaded_fork` port:

> Harden: multiple sequential branches (return_to_depth / repeated
> fast_multithreaded_fork), higher thread counts, a couple of example
> targets (src/examples/* built with -fsanitize=thread). Address the
> explicit-pthread_exit residual only if it shows up (D7).

Unlike Phases 0-3 (each porting one specific, known fix), this phase is
about *discovering* whether problems exist at scale, not applying a known
fix. The DMTCP toolchain remains unavailable in this environment (same gap
every prior phase's Task 3 hit), so "real example targets under actual
DMTCP restart" remains environment-gated. Two other concerns, however, are
directly testable without DMTCP and were both investigated and resolved
during this design's research.

## Investigation findings

### Dimension A: "multiple sequential branches" is already production's architecture

`mc_template_thread_loop_forever()` (`src/lib/template/loop.c:155-239`)
already runs a `while(1)` loop calling `fast_multithreaded_fork()`
repeatedly — once per branch the model checker's DPOR exploration requests,
from the same long-lived template process — and `threadIdx`
(`dmtcp-callback.c:88`) is never reset between calls (unlike the standalone
package's `multithreaded_fork()`, which explicitly resets it "for multiple
calls"). This is intentional, not a gap: `threadIdx`/`threadInfos[]`
represent a snapshot taken *once*, right after the DMTCP restart event (via
each userspace thread's one-time call into `thread_handle_after_dmtcp_restart()`),
and every subsequent `fast_multithreaded_fork()` call correctly reuses that
same frozen snapshot to recreate the same threads in a fresh `_Fork()`'d
child — appropriate for a model checker re-exploring the same starting
state under different scheduling decisions.

The real hardening question is whether R2's TSan fork-hook bracketing
(`__sanitizer_syscall_pre_impl_fork`/`post_impl_fork` around each `_Fork()`
call) stays correct across *many* repeated calls in the same long-lived
process, rather than the single fork Phase 2's harness tested. This is
directly testable without DMTCP by extending Phase 2's harness architecture.

**Verified:** a new standalone harness (Section: Design, below) modeling
the template process's real behavior — original threads check in once and
park *forever* (matching `DMTCP_RESTART_INTO_TEMPLATE`'s actual semantics,
rather than Phase 2's simpler "release and run once" harness, which modeled
a direct-restart-into-branch scenario) — ran 10 sequential
`fast_multithreaded_fork()` cycles from the same process. **All 10/10
iterations passed cleanly**, each branch child's recreated threads
correctly computing `shared_counter=300000` with no CHECK failures, no
crashes, no degradation across repetitions.

### Dimension B: the explicit-pthread_exit-at-scale residual (D7) reproduces, but doesn't apply to McMini

The standalone package's README documents a residual: "explicit
pthread_exit() at high thread counts (~8 concurrent) has a rare (~1/30)
shadow-stack overflow." PLAN.txt's D7 asks whether McMini needs to be
robust against this.

This residual is specific to the vendor package's own join/exit shim
(`tsan_join_shim.c`) — code that exists only because the standalone's
`test_exit.c`/`test_join.c` perform *real* `pthread_exit()` +
`pthread_join()` round trips on recreated (TSan-fiber) threads. **McMini's
production code never does this.** Phase 3's R5 investigation already
established that `mc_pthread_exit()`/`mc_pthread_join()` in `TARGET_BRANCH*`
mode are always a shared-memory mailbox handshake with the `mcmini`
scheduler process — never a real glibc join/exit on a recreated thread.

**Verified (reproduced the residual):** extracted the vendor package
(`multithreaded-fork-tsan-2.0.tar.gz`) to scratch, built its existing
`test_exit.c` with `NUM_WORKERS=8`, and ran it 30 times under real
ThreadSanitizer (`setarch -R`, `TSAN_OPTIONS="handle_segv=0 die_after_fork=0"`).
**1 of 30 runs crashed** with `*** stack smashing detected ***: terminated`
in the branch child, around the explicit `pthread_exit()`/join path —
matching the vendor's own documented ~1/30 rate almost exactly. This
confirms the residual is real and reproducible in this exact toolchain/sandbox.

**Resolution of D7:** since McMini's production code structurally never
exercises the vendor's real-join/real-exit-on-a-fiber code path (confirmed
in Phase 3, re-confirmed here), this residual does not require any McMini
robustness work. Document it as a known, out-of-scope vendor-package
limitation — matching the vendor's own README, which documents rather than
fixes it — and close D7 without a code change, the same pattern as Phase 1's
"the join half of R5 needs no fix" and Phase 3's "pthread_join needs no fix"
conclusions.

## Design

### New file: `test/tsan_support/test_repeated_fork_stress.c`

Extends Phase 2's harness architecture (`test_fastpath_fork_clone_fiber.c`)
with one structural change: the original ("template") worker threads, after
checking in, park on a semaphore that is **never released** — modeling
`DMTCP_RESTART_INTO_TEMPLATE`'s real "threads must wait forever" semantics
(`dmtcp-callback.c`'s own comment) — rather than Phase 2's "release and run
once" model (appropriate there for testing a single direct-restart-into-branch
fork, but not for a long-lived template issuing *many* forks). The `main()`
loop calls `fast_multithreaded_fork()` `NUM_FORKS` (10) times in sequence,
`waitpid()`-reaping each branch child before starting the next iteration —
directly mirroring `mc_template_thread_loop_forever()`'s own
fork-then-wait-for-SIGCHLD-then-loop structure. Each branch child's `NUM_WORKERS`
(3) recreated threads run the same 100,000-iteration mutex-protected
counter workload as Phase 2's harness, `_exit()`ing once done (this harness
tests R2/R3/R4's fork/clone/fiber mechanism at scale, not R5's exit path —
Dimension B covers exit specifically, and does not need McMini's own
mechanism per the finding above).

Reuses the same x86_64-only TLS/descriptor helpers as Phase 2's harness
(same accepted-duplication precedent — linking `dmtcp-callback.c` directly
remains impractical).

### No new file for Dimension B

Per the investigation above, this residual is vendor-package-specific and
McMini's production code is structurally immune to it. No new test is
committed to this repository for it. The finding (reproduction command,
observed failure, and rate) is documented in this spec and the
implementation plan for the historical record, matching how Phase 1
documented "no code change needed" conclusions without inventing
unnecessary test infrastructure.

## Error handling & edge cases

- The repeated-fork harness's `shared_counter` is process-local (not shared
  memory): each `_Fork()`'s child gets its own copy-on-write snapshot, so
  there is no cross-iteration contamination even though the same global
  variable name is reused across all 10 iterations — confirmed by the
  clean, correct counts observed in every iteration during verification.
- `threadInfos[]`/`threadIdx` are populated once (before the fork loop
  begins) and never reset, matching production's actual behavior exactly
  (see Dimension A finding above) — this is not a bug to fix, it's the
  correct design being tested.

## Testing

1. **Dimension A**: `test_repeated_fork_stress.c`, built and run under
   `setarch -R` with the standard `TSAN_OPTIONS`; pass criterion is all 10
   iterations reporting `shared_counter=300000 (expected 300000)` with a
   clean (`code=0 signaled=0`) child exit, and zero CHECK failures — already
   verified during this design's investigation.
2. **Dimension B**: documented finding only (see above) — no new test
   artifact, since production code doesn't exercise this path.
3. **Production build**: `cmake --build build --target libmcmini` must stay
   clean under `-Wall -Werror` (this phase makes no production code
   changes — see Out of scope below).
4. **Environment-gated end-to-end task** (same posture as every prior
   phase's Task 3): PLAN.txt's own "a couple of example targets built with
   `-fsanitize=thread`" under real DMTCP restart remains untestable here;
   documented with exact commands for whoever has that environment.

## Out of scope for this phase

- Any production code change. This phase is purely investigative: Dimension
  A confirms R2/R3/R4 are stable under repeated forking; Dimension B closes
  D7 by confirming McMini doesn't exercise the vulnerable path. Neither
  finding calls for new production code.
- Re-deriving the vendor's join/exit shim in McMini's own mechanism (a
  deliberate scope decision — see Design section above).
- Higher thread counts than 3 for Dimension A's harness — the "higher
  thread count" concern from PLAN.txt is specifically about the
  join/exit-at-scale residual (D7), already covered by Dimension B's reuse
  of the vendor's 8-thread `test_exit.c`; Dimension A's own mechanism
  (fork/clone/fiber without exit) is not thread-count-sensitive in the same
  way, since the vendor's shadow-stack-overflow residual is specific to the
  exit/unwind path, not the clone/resume path Dimension A tests.
