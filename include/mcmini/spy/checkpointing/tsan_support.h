#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <sys/types.h>

/**
 * Returns nonzero if thread `tid` currently has signal `signo` blocked, per
 * the "SigBlk" field of /proc/self/task/<tid>/status. Returns 0 if `tid`
 * cannot be inspected (e.g. it has already exited).
 */
int thread_blocks_signal(pid_t tid, int signo);

/**
 * Returns the calling thread's real (unvirtualized) kernel tid. DMTCP
 * virtualizes gettid() -- including the raw syscall(SYS_gettid) path -- so
 * neither can be used here. Implemented via the kernel's /proc/thread-self
 * magic symlink, whose target string is populated by the kernel itself and
 * is not subject to userspace interception. This is a placeholder: replace
 * the implementation with a DMTCP-provided "real tid" utility if/when one
 * becomes available, without needing to change any caller.
 */
pid_t mc_real_tid(void);

/**
 * Returns true if the calling thread is an "external" thread as described
 * for EXTERNAL_THREAD in record.h -- currently, specifically, ThreadSanitizer's
 * own internal background thread (identified via thread_blocks_signal(),
 * since that thread blocks all signals at creation). The result is computed
 * once per thread and cached in thread-local storage, since the property
 * being tested does not change over a thread's lifetime and the underlying
 * check is not cheap enough to repeat on every call.
 */
bool mc_is_current_thread_tsan_internal(void);

#ifdef __cplusplus
}
#endif
