// Originally written as a bisection variant of exit-stress.c, to isolate
// whether the DMTCP restoreBuf={0,0} race needs the wrapped mutex/
// pthread_exit calls specifically, or just needs 4 libmcmini-wrapped
// threads. It turned out NOT to be useful for that (see conversation): this
// binary instead reliably (4/4 attempts) crashes much earlier, at
// library-load time, before main() or any worker code ever runs -- a
// genuine race between DMTCP's checkpoint-thread creation and TSAN's own
// background-thread spawning, both interacting with libmcmini's lazy-init
// safety net (every wrapped pthread/sem call unconditionally calls
// libmcmini_init(), which takes a pthread_once guard). Concretely: the
// just-created checkpoint thread calls a wrapped sem_post() essentially
// immediately, whose libmcmini_init() safety check crashes inside TSAN's
// own __tsan::SlotLock, apparently because TSAN's own per-thread setup for
// this brand-new thread hasn't finished yet. The original exit-stress.c
// (mutex + explicit pthread_exit) does NOT hit this in the same testing
// session -- with ASLR disabled, a different binary's code/data layout
// shifts the race's timing enough to reliably resolve the other way, even
// though the underlying mechanism is presumably the same for both. Kept as
// a *reliable* reproducer for this crash (much easier to work with than the
// original, rarely-reproducing "elusive heisenbug" noted elsewhere in
// project history), not as a bisection tool.
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define NUM_THREADS 4

void *worker(void *arg) {
  int id = *(int *)arg;
  sleep(10);
  printf("Worker %d: done\n", id);
  pthread_exit(NULL);
}

int main(void) {
  pthread_t threads[NUM_THREADS];
  int ids[NUM_THREADS];

  for (int i = 0; i < NUM_THREADS; i++) {
    ids[i] = i;
    pthread_create(&threads[i], NULL, worker, &ids[i]);
  }
  for (int i = 0; i < NUM_THREADS; i++) {
    pthread_join(threads[i], NULL);
  }

  return 0;
}
