#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <semaphore.h>
#include <stdint.h>

typedef struct {
  // Waited on by the verifier (mcmini), posted by the target thread. The
  // verifier is never checkpointed, so a plain glibc sem_t is safe here.
  sem_t model_side_sem;
  // Waited on by the target thread, posted by the verifier. A DMTCP-restored
  // target thread can be genuinely blocked (kernel-level) on this exact
  // futex word while glibc's own userspace "is anyone really waiting"
  // bookkeeping is desynced from that -- because this memory gets
  // reinitialized (see mc_runner_mailbox_init()/_destroy()) before every new
  // DMTCP-restarted branch, and glibc's NPTL sem_t packs that bookkeeping
  // into the same memory sem_post() checks to decide whether to skip the
  // underlying futex(FUTEX_WAKE) syscall. A plain futex word (see
  // mc_wake_thread()/mc_wait_for_scheduler() in runner_mailbox.c, which
  // always call FUTEX_WAKE unconditionally) has no such bookkeeping to
  // desync.
  uint32_t child_side_sem;
  uint32_t type;
  uint8_t cnts[64];  // TODO: How much space should each thread have to write
                     // payloads?
} runner_mailbox, *runner_mailbox_ref;

void mc_runner_mailbox_init(volatile runner_mailbox *);
void mc_runner_mailbox_destroy(volatile runner_mailbox *);
int mc_wait_for_thread(volatile runner_mailbox *);
int mc_wait_for_scheduler(volatile runner_mailbox *);
int mc_wake_thread(volatile runner_mailbox *);
int mc_wake_scheduler(volatile runner_mailbox *);

#ifdef __cplusplus
}
#endif  // extern "C"
