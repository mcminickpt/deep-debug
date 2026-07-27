#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#define MCMINI_LOG_MINIMUM_LEVEL (MCMINI_LOG_DEBUG)

enum log_level {
  MCMINI_LOG_VERBOSE,
  MCMINI_LOG_DEBUG,
  MCMINI_LOG_INFO,
  MCMINI_LOG_WARNING,
  MCMINI_LOG_ERROR,
  MCMINI_LOG_FATAL,
  MCMINI_LOG_DISABLE
};

#define RELATIVE_PATH(file) ((char*)(file) + (sizeof(LOGGING_ROOT) - 1))
#define __RELATIVE_FILE__ RELATIVE_PATH(__FILE__)

#define log_verbose(...) \
  mcmini_log(MCMINI_LOG_VERBOSE, __RELATIVE_FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) mcmini_log(MCMINI_LOG_DEBUG, __RELATIVE_FILE__, __LINE__, __VA_ARGS__)
#define log_info(...) mcmini_log(MCMINI_LOG_INFO, __RELATIVE_FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...) mcmini_log(MCMINI_LOG_WARNING, __RELATIVE_FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) mcmini_log(MCMINI_LOG_ERROR, __RELATIVE_FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) mcmini_log(MCMINI_LOG_FATAL, __RELATIVE_FILE__, __LINE__, __VA_ARGS__)

void mcmini_log_set_level(int level);
void mcmini_log_toggle(bool enable);
void mcmini_log(int level, const char *file, int line, const char *fmt, ...);

// Force log_mut back to the unlocked state. Call this exactly once, as the
// sole surviving thread in a freshly-_Fork()-ed child, before any other
// thread is recreated in it. See the comment above log_mut in log.c for why
// this is needed.
void mcmini_log_reset_after_fork(void);
