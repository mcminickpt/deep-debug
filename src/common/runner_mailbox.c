#include "mcmini/real_world/mailbox/runner_mailbox.h"

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "mcmini/defines.h"
#include "mcmini/spy/intercept/interception.h"
#include "string.h"

// child_side_sem's own raw-futex protocol (see runner_mailbox.h for why it
// isn't a glibc sem_t): a plain futex word used as a counting semaphore,
// always waking unconditionally on post, so there's no userspace "is anyone
// really waiting" bookkeeping to desync.
static long mc_futex(volatile uint32_t *uaddr, int futex_op, uint32_t val) {
  return syscall(SYS_futex, uaddr, futex_op, val, NULL, NULL, 0);
}

static int mc_raw_sem_wait(volatile uint32_t *sem) {
  while (1) {
    uint32_t cur = __atomic_load_n(sem, __ATOMIC_SEQ_CST);
    if (cur > 0) {
      uint32_t expected = cur;
      if (__atomic_compare_exchange_n(sem, &expected, cur - 1, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        return 0;
      }
      continue;
    }
    errno = 0;
    long rc = mc_futex(sem, FUTEX_WAIT, 0);
    if (rc == -1 && errno != EAGAIN && errno != EINTR) {
      return -1;
    }
  }
}

static int mc_raw_sem_post(volatile uint32_t *sem) {
  __atomic_fetch_add(sem, 1, __ATOMIC_SEQ_CST);
  mc_futex(sem, FUTEX_WAKE, 1);
  return 0;
}

void mc_runner_mailbox_init(volatile runner_mailbox* r) {
  runner_mailbox_ref ref = (runner_mailbox_ref)(r);
#ifdef MC_SHARED_LIBRARY
  libpthread_sem_init(&ref->model_side_sem, SEM_FLAG_SHARED, 0);
#else
  sem_init(&ref->model_side_sem, SEM_FLAG_SHARED, 0);
#endif
  __atomic_store_n(&ref->child_side_sem, 0, __ATOMIC_SEQ_CST);
  memset(ref->cnts, 0, sizeof(ref->cnts));
}

void mc_runner_mailbox_destroy(volatile runner_mailbox* r) {
  runner_mailbox_ref ref = (runner_mailbox_ref)(r);
#ifdef MC_SHARED_LIBRARY
  libpthread_sem_destroy(&ref->model_side_sem);
#else
  sem_destroy(&ref->model_side_sem);
#endif
  // child_side_sem is a plain futex word, not a glibc sem_t: nothing to
  // destroy.
}

int mc_wait_for_thread(volatile runner_mailbox* r) {
  runner_mailbox_ref ref = (runner_mailbox_ref)(r);
#ifdef MC_SHARED_LIBRARY
  return libpthread_sem_wait(&ref->model_side_sem);
#else
  return sem_wait(&ref->model_side_sem);
#endif
}

int mc_wait_for_scheduler(volatile runner_mailbox* r) {
  runner_mailbox_ref ref = (runner_mailbox_ref)(r);
  return mc_raw_sem_wait(&ref->child_side_sem);
}

int mc_wake_thread(volatile runner_mailbox* r) {
  runner_mailbox_ref ref = (runner_mailbox_ref)(r);
  return mc_raw_sem_post(&ref->child_side_sem);
}

int mc_wake_scheduler(volatile runner_mailbox* r) {
  runner_mailbox_ref ref = (runner_mailbox_ref)(r);
#ifdef MC_SHARED_LIBRARY
  return libpthread_sem_post(&ref->model_side_sem);
#else
  return sem_post(&ref->model_side_sem);
#endif
}
