// Stress test for pthread_exit() robustness at higher thread counts under
// DMTCP+TSan restart. doc/pthread-exit-abort-and-fiber-crash.txt and
// multithreaded-fork-tsan-2.0's own README flag a rare (~1-in-30) fiber
// shadow-call-stack overflow specifically at 8+ concurrently-exiting
// threads -- never exercised at this thread count in this repo before
// (every other -exit target here uses 1-2 threads).
//
// NUM_THREADS workers each sleep briefly (giving a checkpoint interval,
// `mcmini -i N`, a reliable window to land while every thread is still
// alive, pre-exit), do a small amount of real, TSan-instrumented work,
// then terminate via an explicit pthread_exit() call. After a
// --multithreaded-fork restart, all NUM_THREADS recreated threads
// (clone()+setcontext()'d onto a fresh TSan fiber -- see
// mc_pthread_is_recreated_thread() in dmtcp-callback.c) hit
// mc_pthread_exit()'s recreated-thread path concurrently.
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define NUM_THREADS 4

pthread_mutex_t counter_mutex;
int counter = 0;

void *worker(void *arg) {
  int id = *(int *)arg;
  sleep(10); // original bug-reproduction timing: short enough that a tight
             // -i 5 checkpoint interval used to race against DMTCP's own
             // ProcessInfo::init() and lose (restoreBuf={0,0}) at 4+ threads.
  pthread_mutex_lock(&counter_mutex);
  counter++;
  printf("Worker %d: counter now %d\n", id, counter);
  pthread_mutex_unlock(&counter_mutex);
  pthread_exit(NULL);
}

int main(void) {
  pthread_t threads[NUM_THREADS];
  int ids[NUM_THREADS];

  pthread_mutex_init(&counter_mutex, NULL);

  for (int i = 0; i < NUM_THREADS; i++) {
    ids[i] = i;
    pthread_create(&threads[i], NULL, worker, &ids[i]);
  }
  for (int i = 0; i < NUM_THREADS; i++) {
    pthread_join(threads[i], NULL);
  }

  return 0;
}
