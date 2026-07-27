#include "mcmini/lib/log.h"
#include "mcmini/spy/intercept/interception.h"

#include <pthread.h>
#include <stdarg.h>
#include <sys/time.h>
#include <unistd.h>
#include "dmtcp.h"

static int global_log_level = MCMINI_LOG_MINIMUM_LEVEL;
static const char *log_level_strs[] = {
  "VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "DISABLE"
};

// mcmini_log() is called concurrently, unsynchronized, by any thread. Its
// call to localtime() below can trigger glibc's lazy timezone-database
// initialization (tzset_internal), which is not safe to race across
// threads on its first call. Constructors run once, before main(), with
// only a single thread, so calling tzset() here forces that lazy init to
// happen safely ahead of any concurrent mcmini_log() call.
__attribute__((constructor)) static void mcmini_log_init_tz(void) {
  tzset();
}

// TSan's public annotation API (see <sanitizer/tsan_interface.h>), used
// below to tell TSan about log_mut's happens-before edge. libmcmini.so
// itself is normally uninstrumented, and log_mut is locked/unlocked via
// libpthread_mutex_lock/unlock, which resolve straight to libpthread's own
// symbols (bypassing TSan's interceptor entirely -- see
// mc_load_intercepted_pthread_functions() in interception.c) -- so without
// this, a real, correctly-held lock is still invisible to TSan, and it
// reports a false race on glibc's internal tzset_internal() state instead.
// Declared weak (same idiom as dmtcp_mcmini_plugin_is_loaded() in main.c),
// so this is a no-op when the process has no TSan runtime loaded at all.
extern void __tsan_acquire(void *addr) __attribute__((weak));
extern void __tsan_release(void *addr) __attribute__((weak));

typedef struct log_record {
  int level;
  int line;
  const char *file;
  const char *format;
  struct tm *time;
  va_list var_args;
  FILE *target;
} log_record;

void mcmini_log_set_level(int level) {
    static pthread_mutex_t level_mut = PTHREAD_MUTEX_INITIALIZER;
    libpthread_mutex_lock(&level_mut);
    global_log_level = level;
    libpthread_mutex_unlock(&level_mut);
}

void mcmini_log_toggle(bool enable) {
    mcmini_log_set_level(MCMINI_LOG_DISABLE);
}

// glibc's tzset_internal() (invoked by localtime_r() below on every call,
// not just the first) is not safe to run concurrently from multiple threads
// even when TZ never changes -- it races on its own internal TZ database
// cache. mcmini_log() is called unsynchronized from any thread, so
// serialize the whole body with this lock.
//
// multithreaded_fork()'s process-duplicating step is a raw _Fork(), chosen
// specifically to skip the work a real fork() would do -- including running
// any registered pthread_atfork() handlers. That means this lock gets no
// fork-safety net: if some other thread (most plausibly the template
// thread, which logs constantly) holds it at the exact instant _Fork()
// snapshots memory for a new branch, every thread in that branch that later
// calls mcmini_log() deadlocks on it forever -- the true owner's
// continuation isn't part of this child's process at all, so nothing can
// ever unlock it. See doc/log-mutex-fork-desync.txt.
static pthread_mutex_t log_mut = PTHREAD_MUTEX_INITIALIZER;

void mcmini_log_reset_after_fork(void) {
  // Must use the bypass handle, not the plain pthread_mutex_init(): that
  // symbol is libmcmini's own interposed mc_pthread_mutex_init(), which
  // dispatches on get_current_mode() and (in TARGET_BRANCH_AFTER_RESTART/
  // DMTCP_RESTART_INTO_BRANCH modes) asserts this thread already has a
  // valid tid_self via thread_get_mailbox() -- not yet true this early,
  // right after _Fork(), before restart_child_threads_fast() runs. log_mut
  // is a purely internal implementation detail, never meant to be
  // model-checked in the first place.
  libpthread_mutex_init(&log_mut, NULL);
}

void mcmini_log(int level, const char *file, int line, const char *fmt, ...) {
  if (level < global_log_level) {
      return;
  }
  libpthread_mutex_lock(&log_mut);
  if (__tsan_acquire) __tsan_acquire(&log_mut);

  log_record rc;
  time_t t = time(NULL);
  rc.format = fmt;
  rc.file = file;
  rc.line = line;
  rc.level = level;
  rc.target = stdout;
  struct tm tm_buf;
  localtime_r(&t, &tm_buf);
  rc.time = &tm_buf;
  va_start(rc.var_args, fmt);
  char buf[20];
  buf[strftime(buf, sizeof(buf), "%H:%M:%S", rc.time)] = '\0';
  const pid_t pid = mcmini_real_pid(getpid());
  fprintf(
    rc.target,
    "[%u] %s %-5s %s:%d: ", pid,
    buf, log_level_strs[rc.level], rc.file, rc.line
  );
  vfprintf(rc.target, rc.format, rc.var_args);
  fprintf(rc.target, "\n");
  fflush(rc.target);
  va_end(rc.var_args);

  if (__tsan_release) __tsan_release(&log_mut);
  libpthread_mutex_unlock(&log_mut);
}
