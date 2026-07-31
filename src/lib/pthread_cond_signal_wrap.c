// Like pthread_cond_wait_wrap.c: must be compiled into the TARGET binary
// itself (not libmcmini.so), linked with `-Wl,--wrap=pthread_cond_signal`.
//
// TSan's pthread_cond_signal interceptor has the same interposition-chain
// bypass as pthread_cond_wait's -- confirmed live, contrary to
// doc/cond-wait-tsan-interceptor-bypass.txt's original assumption that
// signal/broadcast delegate normally. Without this wrap,
// mc_pthread_cond_signal() is never reached under DMTCP.
//
// No __real_pthread_cond_signal() fallback needed: mc_pthread_cond_signal()
// already handles every libmcmini_mode itself.

#include <pthread.h>

extern int mc_pthread_cond_signal(pthread_cond_t *cond);

int __wrap_pthread_cond_signal(pthread_cond_t *cond) {
  return mc_pthread_cond_signal(cond);
}
