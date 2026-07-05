// Standalone host-side unit test. Not an mcmini model-checking target.
//
// Proves the dlopen+dlsym technique Task 2 uses to add a cached "real
// pthread_exit" handle to src/lib/interception.c (matching the existing
// libpthread_pthread_join_ptr/libpthread_timedjoin_np_ptr pattern): resolve
// pthread_exit from a freshly dlopen'd libpthread.so/libpthread.so.0, call
// it from a thread, and confirm the retval reaches a real pthread_join().
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>

typedef void (*pthread_exit_fn)(void *);

static pthread_exit_fn real_pthread_exit;

static void *worker(void *arg) {
  (void)arg;
  real_pthread_exit((void *)(long)42);
  return NULL; // not reached
}

int main(void) {
  void *libpthread_handle = dlopen("libpthread.so", RTLD_LAZY);
  if (!libpthread_handle) {
    libpthread_handle = dlopen("libpthread.so.0", RTLD_LAZY);
  }
  assert(libpthread_handle != NULL);

  real_pthread_exit = (pthread_exit_fn)dlsym(libpthread_handle, "pthread_exit");
  assert(real_pthread_exit != NULL);

  pthread_t t;
  int rc = pthread_create(&t, NULL, worker, NULL);
  assert(rc == 0);

  void *retval = NULL;
  rc = pthread_join(t, &retval);
  assert(rc == 0);
  assert((long)retval == 42);

  printf("PASS\n");
  return 0;
}
