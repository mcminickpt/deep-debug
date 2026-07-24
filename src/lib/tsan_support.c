#include "mcmini/spy/checkpointing/tsan_support.h"

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "mcmini/defines.h"

// Matches dmtcp-callback.c's / multithreaded_fork.c's own definition. Not
// shared via a common header since, today, this is the only other file that
// needs it; worth hoisting to a shared header if a fourth user shows up.
#define SIG_MULTITHREADED_FORK (SIGRTMIN + 6)

// Both functions below may run on a thread ThreadSanitizer has not yet
// registered (i.e. before that OS thread's TSan trampoline has executed) --
// specifically, mc_is_current_thread_tsan_internal() is called from
// get_current_mode(), which runs on every wrapped call, including a brand
// new worker thread's very first one. Empirically isolated by bisecting this
// file's original fopen/fgets/sscanf/fclose-based implementation one call at
// a time: raw syscalls for openat/read/readlink are safe here, but even a
// raw syscall(SYS_close, fd) crashes (null ThreadState dereference inside
// libtsan), so the one fd this code opens is deliberately never closed (see
// below). Buffered stdio and sscanf/snprintf were replaced with hand-rolled
// equivalents in the course of this isolation, not because they were
// individually confirmed unsafe.

int thread_blocks_signal(pid_t tid, int signo) {
  (void)signo;
  char path[64];
  {
    const char *prefix = "/proc/self/task/";
    const char *suffix = "/status";
    size_t i = 0;
    for (const char *p = prefix; *p; p++) path[i++] = *p;
    char digits[16];
    int nd = 0;
    unsigned int v = (tid < 0) ? 0 : (unsigned int)tid;
    if (v == 0) digits[nd++] = '0';
    while (v > 0) { digits[nd++] = (char)('0' + (v % 10)); v /= 10; }
    while (nd > 0) path[i++] = digits[--nd];
    for (const char *p = suffix; *p; p++) path[i++] = *p;
    path[i] = '\0';
  }

  int fd = syscall(SYS_openat, AT_FDCWD, path, O_RDONLY);
  if (fd < 0) {
    return 0;
  }

  char buf[4096];
  ssize_t total = 0;
  ssize_t n;
  while (total < (ssize_t)sizeof(buf) - 1 &&
         (n = syscall(SYS_read, fd, buf + total, sizeof(buf) - 1 - total)) > 0) {
    total += n;
  }
  // Deliberately not closed: empirically, even a raw syscall(SYS_close, fd)
  // crashes here (unlike openat/read/readlink), when called -- as this
  // function can be -- from a thread ThreadSanitizer has not yet
  // registered. Leaking this one fd is an acceptable, bounded cost: this
  // path only ever runs once per thread for the process's entire lifetime
  // (see mc_is_current_thread_tsan_internal()'s caching).
  buf[total] = '\0';

  const char *sigblk_line = strstr(buf, "SigBlk:");
  if (sigblk_line == NULL) {
    return 0;
  }
  const char *p = sigblk_line + strlen("SigBlk:");
  while (*p == ' ' || *p == '\t') p++;
  unsigned long long sigblk = 0;
  int any_digit = 0;
  while (*p) {
    int d;
    if (*p >= '0' && *p <= '9') d = *p - '0';
    else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
    else break;
    sigblk = (sigblk << 4) | (unsigned long long)d;
    any_digit = 1;
    p++;
  }
  if (!any_digit) {
    return 0;
  }
  return (int)((sigblk >> (signo - 1)) & 1ULL);
}

pid_t mc_real_tid(void) {
  char linkbuf[64];
  ssize_t n = syscall(SYS_readlink, "/proc/thread-self", linkbuf,
                      sizeof(linkbuf) - 1);
  if (n < 0) {
    return -1;
  }
  linkbuf[n] = '\0';
  const char *task = strstr(linkbuf, "/task/");
  if (task == NULL) {
    return -1;
  }
  return (pid_t)atoi(task + strlen("/task/"));
}

bool mc_is_current_thread_tsan_internal(void) {
  // -1: not yet computed; 0: no; 1: yes. Cached per-thread since the
  // property being tested never changes over a thread's lifetime, and
  // thread_blocks_signal() is not cheap enough to call on every wrapped op.
  static MCMINI_THREAD_LOCAL int cached = -1;
  if (cached == -1) {
    cached = thread_blocks_signal(mc_real_tid(), SIG_MULTITHREADED_FORK) ? 1 : 0;
  }
  return cached == 1;
}
