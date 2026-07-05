// Standalone host-side unit test for thread_blocks_signal(). Not an mcmini
// model-checking target: compile and run directly (see the command in the
// implementation plan / commit message), not through CMake.
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "mcmini/spy/checkpointing/tsan_support.h"

static sem_t ready;
static pid_t worker_tid;

static void *worker(void *arg) {
  (void)arg;
  worker_tid = (pid_t)syscall(SYS_gettid);

  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &set, NULL);

  sem_post(&ready);

  for (;;) {
    pause();  // cancellation point; SIGUSR1 stays blocked the whole time
  }
  return NULL;
}

int main(void) {
  int rc = sem_init(&ready, 0, 0);
  assert(rc == 0);

  pthread_t t;
  rc = pthread_create(&t, NULL, worker, NULL);
  assert(rc == 0);

  rc = sem_wait(&ready);
  assert(rc == 0);

  pid_t self_tid = (pid_t)syscall(SYS_gettid);

  assert(thread_blocks_signal(worker_tid, SIGUSR1) == 1);
  assert(thread_blocks_signal(self_tid, SIGUSR1) == 0);
  assert(thread_blocks_signal(999999 /* bogus tid, should not exist */, SIGUSR1) == 0);

  rc = pthread_cancel(t);
  assert(rc == 0);
  rc = pthread_join(t, NULL);
  assert(rc == 0);

  printf("PASS\n");
  return 0;
}
