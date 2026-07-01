#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// memcpy implementation but with volatile memory
volatile void *memset_v(volatile void *, int ch, size_t n);
volatile void *memcpy_v(volatile void *, const volatile void *, size_t n);

// TSan-safe bump allocator over a fixed static (BSS) arena.
//
// Used for allocations that may execute on a thread BEFORE ThreadSanitizer has
// registered it -- specifically the mc_thread_routine_wrapper prologue when a
// TSAN'd target runs under DMTCP (libtsan wraps libmcmini, so libmcmini's
// wrapper runs before libtsan's thread-start trampoline). It performs NO libc
// call and NO syscall on the fast path -- only a static-memory atomic bump --
// so it never enters a TSan interceptor and never dereferences an
// unregistered thread's (null) ThreadState. Never frees (nodes it backs are
// never freed; see record.c / wrappers.c). See TSAN-McMini-DMTCP.txt.
void *mc_ts_alloc(size_t n);

#ifdef __cplusplus
}  // extern "C"
#endif
