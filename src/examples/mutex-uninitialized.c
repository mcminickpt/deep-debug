// Negative test for PTHREAD_MUTEX_INITIALIZER support.
//
// A stack-allocated mutex that was never initialized -- neither via
// pthread_mutex_init() nor PTHREAD_MUTEX_INITIALIZER -- is locked. McMini must
// report undefined behavior rather than silently accepting it.
//
// The mutex is filled with non-zero garbage so it cannot be mistaken for
// PTHREAD_MUTEX_INITIALIZER (which is all zeros on glibc). This is exactly the
// stack/garbage case the process_vm_readv check is designed to catch; a
// zero-filled data/heap mutex would (intentionally) slip through -- see the
// FIXME in src/mcmini/model/transitions/mutex.cpp.
//
// Expected McMini result:
//   UNDEFINED BEHAVIOR:
//   Attempting to lock an uninitialized mutex
#include <pthread.h>
#include <string.h>

int main(void) {
  pthread_mutex_t mutex;
  memset(&mutex, 0xff, sizeof(mutex));  // simulate an uninitialized stack mutex
  pthread_mutex_lock(&mutex);
  return 0;
}
