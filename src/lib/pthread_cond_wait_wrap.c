// Like pthread_join_wrap.c: must be compiled into the TARGET binary itself
// (not libmcmini.so), linked with `-Wl,--wrap=pthread_cond_wait`.
//
// Unlike pthread_join/mutex_lock/sem_wait, TSan's pthread_cond_wait
// interceptor never delegates to the next implementation in the
// interposition chain -- it implements the wait atomically inside its own
// runtime. So under DMTCP, mc_pthread_cond_wait() is never reached at all
// without this wrap. See doc/cond-wait-tsan-interceptor-bypass.txt.
//
// No __real_pthread_cond_wait() fallback needed here (unlike
// __wrap_pthread_join()): mc_pthread_cond_wait() already handles every
// libmcmini_mode itself.

#include <pthread.h>

extern int mc_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);

int __wrap_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  return mc_pthread_cond_wait(cond, mutex);
}
