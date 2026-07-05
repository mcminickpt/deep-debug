# Design: TSan Port Phase 1 (R1) — Exclude TSan-internal threads from the restart barrier

Date: 2026-07-04
Related: `PLAN.txt` (Phase 1, fix R1), branch `tsan-multithreaded-fork-port`,
baseline commit `b38da82` (Phase 0 cleanup).

## Problem

`template_thread()` in `src/lib/dmtcp-callback.c` computes a thread-count
barrier at DMTCP restart:

```c
thread_count = (entries in /proc/self/task) - 2;   // self + checkpoint thread
for (int i = 0; i < thread_count; i++)
  libpthread_sem_wait_loop(&dmtcp_restart_sem);
```

Each userspace thread posts to `dmtcp_restart_sem` lazily, from inside
`thread_handle_after_dmtcp_restart()`, which only runs when that thread's next
libpthread call passes through one of libmcmini's own wrappers
(`sem-wrappers.c`, `wrappers.c`) and notices `is_in_restart_mode()`. There is
no active signal broadcast to every thread in this fast path (unlike the
standalone `multithreaded-fork-tsan-2.0` recipe, which uses a real signal
barrier).

When the target is built with ThreadSanitizer, libtsan spawns its own
background thread at process start with all signals blocked. That thread
never calls a wrapped libpthread function, so it never reaches
`thread_handle_after_dmtcp_restart()` and never posts to `dmtcp_restart_sem` —
but it *is* counted in `/proc/self/task`, inflating `thread_count` by one.
`template_thread()` then waits forever for a post that will never come: the
"stabilization hang" described in `PLAN.txt` section 2/4 (D1).

## Design

### Files

- New `src/lib/tsan_support.c` + `include/mcmini/spy/checkpointing/tsan_support.h`
  (alongside the existing `record.h`, `alloc.h`, etc. in that directory).
  This phase adds one function; later phases (R3 `__clone`, R4 fiber, R5
  join/exit) add their helpers to the same pair of files.
- `src/lib/tsan_support.c` added to the `libmcmini` source list in
  `CMakeLists.txt`, alphabetically between `template/sig.c` and `wrappers.c`.

### `tsan_support.c`: `thread_blocks_signal()`

```c
int thread_blocks_signal(pid_t tid, int signo);
```

Direct port of the standalone's helper
(`multithreaded-fork-tsan-2.0/multithreaded_fork.c:323-337`): opens
`/proc/self/task/<tid>/status`, parses the `SigBlk:` hex mask, and returns
whether bit `signo - 1` is set. Returns `0` (not blocked) if the status file
can't be opened (e.g. the thread already exited) — matching the standalone's
behavior; this is a pre-existing raciness in the directory scan, not
introduced by this change.

Used as a probe, not a real signal path: nothing in the fast path sends
`SIG_MULTITHREADED_FORK` (`SIGRTMIN+6`, already `#define`d in
`dmtcp-callback.c`, currently otherwise unused there). We only check whether
each thread's `SigBlk` mask has that bit set, which is how libtsan's
background thread — which blocks all signals at creation — gets identified.
Ordinary application threads are not expected to block this reserved
real-time signal.

### `dmtcp-callback.c`: per-tid classification in `template_thread()`

Replace the blanket `thread_count -= 2` with explicit per-tid skips while
walking `/proc/self/task`:

- Skip the tid equal to `syscall(SYS_gettid)` (the template thread itself).
- Skip the checkpoint thread's tid, obtained via a new **read-only**
  `get_tid_from_pthread_descriptor(ckpt_pthread_descriptor)` helper added next
  to the existing `patchThreadDescriptor()` / `pthreadDescriptorTidOffset()`
  in `dmtcp-callback.c` (not `tsan_support.c` — this is generic
  pthread-descriptor-layout infrastructure, not TSan-specific). Unlike
  `patchThreadDescriptor()`, it must not mutate the descriptor; it only reads
  the tid field at the same offset.
- Skip any tid where `thread_blocks_signal(tid, SIG_MULTITHREADED_FORK)` is
  true, logging at debug level which tid was excluded and why.
- Everything else increments `thread_count`, exactly as today.

### threadInfos[] recreation

`PLAN.txt` also asks to ensure TSan-internal threads are excluded from
`threadInfos[]` recreation, not just from the count. Structurally,
`threadInfos[]` is populated only inside `thread_handle_after_dmtcp_restart()`,
which (as above) is reached only via a wrapped libpthread call. TSan's
background thread is not expected to make such a call, so it should never
register itself regardless. No defensive check is added in
`thread_handle_after_dmtcp_restart()` for this phase; this assumption is
verified empirically by the phase's pass criteria below (a crash or hang
caused by attempting to recreate the background thread would surface
immediately in Phase 2 testing when branch children run).

## Error handling & edge cases

- `opendir("/proc/self/task")` failure: unchanged (existing `perror` +
  `mc_exit(EXIT_FAILURE)`).
- `thread_blocks_signal()` on a tid whose status file has already vanished:
  returns `0`, not excluded — pre-existing raciness, out of scope here.
- `/proc/self/task` entries are always numeric tids, so `atoi(entry->d_name)`
  is safe once `.`/`..` are filtered.

## Testing / verification

Matches `PLAN.txt` Phase 1 pass criteria: rebuild `libmcmini`, re-record a
checkpoint of a `-fsanitize=thread` example target (checkpoints must be
re-recorded after any libmcmini rebuild — PLAN.txt D4), then restart with
`--multithreaded-fork`. Success is the `template_thread()` debug log showing
a thread count matching the actual live (non-TSan-internal) thread count,
followed by "threads now in a consistent state" — instead of hanging. A
`log_debug` line names each excluded tid and the reason (self / checkpoint
thread / TSan-internal), making the exclusion directly observable in the log.

No new unit-test harness is introduced — the project has no test harness yet
(per `CLAUDE.md`), and this is a runtime/scheduling fix best verified against
a real TSan target, per PLAN.txt's own phase methodology (section 7).

## Out of scope for this phase

- R2/R3/R4/R5 (Phase 2/3 of `PLAN.txt`) — fork hooks, `__clone`, fiber
  switching, join/exit shims. R2/R4 keeper prototypes already exist in
  `dmtcp-callback.c` from Phase 0 and are untouched here.
- Any defensive `threadInfos[]` registration check (see above) — deferred
  until/unless empirical testing shows it's needed.
- Backporting to `src/common/multithreaded_fork.c` (PLAN.txt Q2) — that file
  is not the live path for deep-debug's restart flow.
