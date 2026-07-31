// NOTE: unlike every other file in src/lib/, this one must NOT be compiled
// into libmcmini.so. It must be compiled directly into the TARGET program's
// own binary, linked alongside the flag `-Wl,--wrap=pthread_join`.
//
// Why: under DMTCP, libtsan.so is loaded ahead of libmcmini.so, so a
// target's own calls to the public pthread_join() symbol reach TSan's
// interceptor first, before libmcmini's. For a thread DMTCP resurrected via
// clone() (bypassing TSan's own pthread_create() interceptor), TSan's
// interceptor can never resolve that thread's Tid and hangs forever -- see
// TSAN-pthread-join.md. `-Wl,--wrap=pthread_join`, applied when linking the
// target itself, sidesteps this entirely: it rewrites the target's own
// pthread_join() calls, at link time, to call __wrap_pthread_join() below
// instead -- before the dynamic linker (and hence TSan's interceptor) is
// even involved. When a real join is safe, this calls __real_pthread_join(),
// which the linker's --wrap rewriting resolves to the original pthread_join
// symbol -- so TSan still gets to see it and do its own happens-before
// bookkeeping for that call, same as it would without any of this.
//
// __real_pthread_join is not a real, independently-resolvable symbol: it
// only exists as a link-time rewrite within this same --wrap=pthread_join
// link, which is why this must be compiled directly into the target and
// cannot live inside libmcmini.so (a separately-built shared library).

#include <pthread.h>
#include <stdbool.h>

extern int __real_pthread_join(pthread_t thread, void **retval);
extern int mc_pthread_join_maybe_defer(pthread_t thread, void **retval,
                                       bool *deferred);

int __wrap_pthread_join(pthread_t thread, void **retval) {
  bool deferred;
  int result = mc_pthread_join_maybe_defer(thread, retval, &deferred);
  if (deferred) {
    return __real_pthread_join(thread, retval);
  }
  return result;
}
