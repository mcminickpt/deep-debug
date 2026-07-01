// Negative test for PTHREAD_COND_INITIALIZER support.
//
// A stack-allocated condition variable that was never initialized -- neither
// via pthread_cond_init() nor PTHREAD_COND_INITIALIZER -- is operated on. McMini
// must report undefined behavior rather than silently accepting it.
//
// The cond var is filled with non-zero garbage so it cannot be mistaken for
// PTHREAD_COND_INITIALIZER (which is all zeros on glibc). This is exactly the
// stack/garbage case the process_vm_readv check is designed to catch; a
// zero-filled data/heap cv would (intentionally) slip through -- see the FIXME
// in src/mcmini/model/transitions/condition_variables.cpp.
//
// Expected McMini result:
//   UNDEFINED BEHAVIOR:
//   Attempting to signal an uninitialized condition variable
#include <pthread.h>
#include <string.h>

int main(void) {
  pthread_cond_t cond;
  memset(&cond, 0xff, sizeof(cond));  // simulate an uninitialized stack cv
  pthread_cond_signal(&cond);
  return 0;
}
