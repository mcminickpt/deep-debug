// Standalone host-side unit test. Not an mcmini model-checking target.
//
// Independently re-verifies the x86_64 pthread_t -> tid offset that
// src/lib/dmtcp-callback.c's pthreadDescriptorTidOffset() uses, specifically
// for READING (not patching) another thread's descriptor -- the new use case
// get_tid_from_pthread_descriptor() introduces. The existing
// patchThreadDescriptor() already self-verifies the same offset for
// `pthread_self()` on every restart (see saveThreadStateBeforeFork()); this
// test covers the "another thread's descriptor" case that path never
// exercises.
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef __x86_64__
#error "This standalone test only covers __x86_64__; see dmtcp-callback.c's pthreadDescriptorTidOffset() for other architectures."
#endif

static pid_t tid_from_descriptor(pthread_t descriptor) {
  const int offset = 720;  // matches pthreadDescriptorTidOffset() for __x86_64__
  return *(pid_t *)((char *)descriptor + offset);
}

static sem_t ready;
static pthread_t worker_self;
static pid_t worker_tid;

static void *worker(void *arg) {
  (void)arg;
  worker_self = pthread_self();
  worker_tid = (pid_t)syscall(SYS_gettid);
  sem_post(&ready);
  for (;;) {
    pause();
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

  // Read another thread's tid from its descriptor without mutating it.
  assert(tid_from_descriptor(worker_self) == worker_tid);
  // Reading twice must be idempotent (unlike patchThreadDescriptor()).
  assert(tid_from_descriptor(worker_self) == worker_tid);

  rc = pthread_cancel(t);
  assert(rc == 0);
  rc = pthread_join(t, NULL);
  assert(rc == 0);

  printf("PASS\n");
  return 0;
}
