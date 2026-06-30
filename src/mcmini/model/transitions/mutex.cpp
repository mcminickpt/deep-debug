#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // for process_vm_readv(2)
#endif
#include <pthread.h>
#include <sys/uio.h>

#include <cstring>

#include "mcmini/mem.h"
#include "mcmini/model/exception.hpp"
#include "mcmini/model/transitions/mutex/callbacks.hpp"

using namespace model;
using namespace objects;

namespace {

/**
 * @brief Reads the bytes of the mutex at `remote_mutex` out of the target
 * process `target_pid` and reports whether they match the bit pattern of a
 * statically-initialized mutex (`PTHREAD_MUTEX_INITIALIZER`).
 *
 * This is how a mutex declared with `PTHREAD_MUTEX_INITIALIZER` (which never
 * flows through the `pthread_mutex_init` wrapper, and so is never observed by
 * the model) is distinguished from a genuinely uninitialized one.
 *
 * @return true if the remote bytes equal `PTHREAD_MUTEX_INITIALIZER`; false if
 * they differ or the remote memory could not be read.
 */
bool remote_mutex_is_static_initializer(pid_t target_pid,
                                        pthread_mutex_t *remote_mutex) {
  if (target_pid <= 0) return false;
  pthread_mutex_t initializer = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_t local;
  struct iovec local_iov = {&local, sizeof(local)};
  struct iovec remote_iov = {(void *)remote_mutex, sizeof(local)};
  ssize_t nread =
      process_vm_readv(target_pid, &local_iov, 1, &remote_iov, 1, 0);
  if (nread != (ssize_t)sizeof(local)) return false;
  return memcmp(&local, &initializer, sizeof(local)) == 0;
}

}  // namespace

model::transition* mutex_init_callback(runner_id_t p,
                                       const volatile runner_mailbox& rmb,
                                       model_to_system_map& m) {
  // Fetch the remote object
  pthread_mutex_t* remote_mut;
  memcpy_v(&remote_mut, (volatile void*)rmb.cnts, sizeof(pthread_mutex_t*));

  // Locate the corresponding model of this object
  if (!m.contains(remote_mut))
    m.observe_object(remote_mut, new mutex(mutex::state::uninitialized));

  state::objid_t const mut = m.get_model_of_object(remote_mut);
  return new transitions::mutex_init(p, mut);
}

model::transition* mutex_lock_callback(runner_id_t p,
                                       const volatile runner_mailbox& rmb,
                                       model_to_system_map& m) {
  pthread_mutex_t* remote_mut;
  memcpy_v(&remote_mut, (volatile void*)rmb.cnts, sizeof(pthread_mutex_t*));

  if (!m.contains(remote_mut)) {
    // The model has never seen this mutex initialized. Either it was declared
    // with `PTHREAD_MUTEX_INITIALIZER` (which never flows through the
    // `pthread_mutex_init` wrapper) or it is genuinely uninitialized. Read the
    // target's memory to tell the two apart.
    //
    // FIXME: This detects an uninitialized-mutex bug only for stack-allocated
    // (or otherwise garbage-filled) mutexes. A mutex in the data or heap
    // segment is zero-initialized by the OS, and on glibc that bit pattern is
    // identical to `PTHREAD_MUTEX_INITIALIZER`, so a never-initialized
    // data/heap mutex passes this check and slips through undetected (same
    // limitation as the original McMini).
    if (!remote_mutex_is_static_initializer(m.get_target_pid(), remote_mut))
      throw undefined_behavior_exception(
          "Attempting to lock an uninitialized mutex");

    // Statically initialized via `PTHREAD_MUTEX_INITIALIZER`: lazily register
    // it as an initialized, unlocked mutex.
    m.observe_object(remote_mut, new mutex(mutex::state::unlocked, remote_mut));
  }

  state::objid_t const mut = m.get_model_of_object(remote_mut);
  return new transitions::mutex_lock(p, mut);
}

model::transition* mutex_unlock_callback(runner_id_t p,
                                         const volatile runner_mailbox& rmb,
                                         model_to_system_map& m) {
  pthread_mutex_t* remote_mut;
  memcpy_v(&remote_mut, (volatile void*)rmb.cnts, sizeof(pthread_mutex_t*));

  // Unlike lock, unlocking a mutex the model has never seen is genuine
  // undefined behavior (a `PTHREAD_MUTEX_INITIALIZER` mutex starts unlocked, so
  // a first-ever operation of "unlock" cannot be valid). This matches the
  // original McMini, which does not lazily register a mutex on unlock.
  if (!m.contains(remote_mut))
    throw undefined_behavior_exception(
        "Attempting to unlock an uninitialized mutex");

  state::objid_t const mut = m.get_model_of_object(remote_mut);
  return new transitions::mutex_unlock(p, mut);
}
