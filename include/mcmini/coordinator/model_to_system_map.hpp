#pragma once

#include <functional>

#include "mcmini/coordinator/coordinator.hpp"
#include "mcmini/forwards.hpp"
#include "mcmini/model/state.hpp"
#include "mcmini/real_world/remote_address.hpp"

/**
 * @brief A mapping between the remote addresses pointing to the C/C++ structs
 * the objects in McMini's model are emulating.
 *
 * As McMini explores different paths of execution of the target program at
 * runtime, it may discover new visible objects. However, visible objects are
 * only a _representation in the McMini model_ of the actual underlying
 * structs containing the information used to implement the primitive. The
 * underlying process, however, refers to these objects as pointers to the
 * multi-threaded primitives (e.g. a `pthread_mutex_t*` in
 * `pthread_mutex_lock()`). McMini therefore needs to maintain a
 * correspondence between these addresses and the identifiers in McMini's
 * model used to represent those objects to the model checker.
 *
 * @important: Handles are assumed to remain valid _across process source
 * invocations_. In the future, we could support the ability to _remap_
 * process handles dynamically during each new re-execution scheduled by
 * the coordinator to handle aliasing etc. by using the trace as a total
 * ordering on object-creation events. Until we run into this issue, we leave it
 * for future development.
 *
 * @note this correspondence covers visible _objects_ only. Runners (threads)
 * are identified by the `runner_id_t` that `libmcmini.so` assigns them, not by
 * the address of their `pthread_t` descriptor, which glibc recycles. See
 * `observe_runner()`.
 */
class model_to_system_map final {
 private:
  coordinator &_coordinator;
  friend coordinator;

 public:
  /*
   * Prevent external construction (only the coordinator can construct
   * instances of this class)
   */
  model_to_system_map(coordinator &coordinator) : _coordinator(coordinator) {}
  model_to_system_map() = delete;

  model::state::objid_t get_model_of_object(
      real_world::remote_address<void>) const;
  bool contains(real_world::remote_address<void> addr) const {
    return get_model_of_object(addr) != model::invalid_objid;
  }

  using runner_generation_function =
      std::function<const model::transition *(model::state::runner_id_t)>;

  /**
   * @brief Record the presence of a new visible object that is
   * represented with the system id.
   *
   * @param remote_process_visible_object_handle the address containing
   * the data for the new visible object across process handles of the
   * `real_world::process_source` in the coordinator
   */
  model::state::objid_t observe_object(real_world::remote_address<void>,
                                       const model::visible_object_state *);

  /**
   * @brief Record the presence of a new runner (thread) which `libmcmini.so`
   * has assigned the id `id` in the target process.
   *
   * Unlike visible objects, runners are _not_ identified by a remote address.
   * A `pthread_t` is the address of a thread descriptor which glibc recycles
   * once a thread is joined, so the same address routinely denotes different
   * threads over the lifetime of the target. Instead, `libmcmini.so` assigns
   * each thread a `runner_id_t` from a monotonic counter and reports that id
   * directly; the same id also indexes the thread's mailbox in shared memory.
   *
   * The model allocates runner ids the same way (in order of discovery), so
   * the id the model assigns must agree with the one the target reports.
   *
   * @throws std::runtime_error if the model assigns an id other than `id`,
   * which means the model and the target have disagreed about the set of
   * live threads.
   */
  void observe_runner(model::state::runner_id_t id,
                      const model::runner_state *);
  void observe_runner(model::state::runner_id_t id, const model::runner_state *,
                      runner_generation_function f);
  void observe_runner_transition(const model::transition *);
};
