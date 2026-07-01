#include "mcmini/mem.h"

#include <stdatomic.h>
#include <sys/syscall.h>
#include <unistd.h>

// See mc_ts_alloc() in mem.h for why this allocator exists and why it must not
// call libc or a wrapped syscall on its fast path.
#define MC_TS_ARENA_SIZE (4u * 1024 * 1024)
static char mc_ts_arena[MC_TS_ARENA_SIZE];
static atomic_size_t mc_ts_off = 0;

void *mc_ts_alloc(size_t n) {
  n = (n + 15u) & ~(size_t)15u;  // 16-byte align
  size_t off = atomic_fetch_add(&mc_ts_off, n);
  if (off + n > MC_TS_ARENA_SIZE) {
    // Exhausted. We may be running before this thread is registered with TSan,
    // so we cannot fall back to malloc (it would enter a TSan interceptor).
    // Fail via raw syscalls only (no interceptors).
    static const char msg[] = "libmcmini: mc_ts_alloc arena exhausted\n";
    syscall(SYS_write, 2, msg, sizeof(msg) - 1);
    syscall(SYS_exit_group, 1);
  }
  return &mc_ts_arena[off];
}

volatile void *memset_v(volatile void *dst, int ch, size_t n) {
  volatile unsigned char *dstc = dst;
  while ((n--) > 0) dstc[n] = ch;
  return dst;
}

volatile void *memcpy_v(volatile void *dst, const volatile void *src,
                        size_t n) {
  // From cppreference on the use of restrict pointers in the C language:
  //
  // | Restricted pointers can be assigned to unrestricted pointers freely, the
  // | optimization opportunities remain in place as long as the compiler is
  // | able to analyze the code:
  // |
  // | void f(int n, float * restrict r, float * restrict s)
  // | {
  // |   float *p = r, *q = s; // OK
  // |    while (n-- > 0)
  // |        *p++ = *q++; // almost certainly optimized just like *r++ = *s++
  // | }
  //
  // See https://en.cppreference.com/w/c/language/restrict for details.
  const volatile unsigned char *srcc = src;
  volatile unsigned char *dstc = dst;
  while ((n--) > 0) dstc[n] = srcc[n];
  return dst;
}
