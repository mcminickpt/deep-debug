// Creates and joins workers one at a time. glibc caches the joined thread's
// stack, so every round hands back the same `pthread_t`. McMini must identify
// each worker by the id `libmcmini.so` assigned it, not by that descriptor.

#include <pthread.h>
#include <stdio.h>

#define ROUNDS 3

static pthread_mutex_t mutex;

static void *worker(void *unused) {
  pthread_mutex_lock(&mutex);
  pthread_mutex_unlock(&mutex);
  return NULL;
}

int main(int argc, char *argv[]) {
  pthread_mutex_init(&mutex, NULL);

  for (int i = 0; i < ROUNDS; i++) {
    pthread_t worker_thread;
    pthread_create(&worker_thread, NULL, &worker, NULL);
    printf("round %d: pthread_t = %p\n", i, (void *)worker_thread);
    pthread_join(worker_thread, NULL);
  }
  return 0;
}
