#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/**
 * Returns nonzero if thread `tid` currently has signal `signo` blocked, per
 * the "SigBlk" field of /proc/self/task/<tid>/status. Returns 0 if `tid`
 * cannot be inspected (e.g. it has already exited).
 */
int thread_blocks_signal(pid_t tid, int signo);

#ifdef __cplusplus
}
#endif
