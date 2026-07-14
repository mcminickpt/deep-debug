#pragma once

#include "mcmini/coordinator/coordinator.hpp"
#include "mcmini/model/exception.hpp"
#include "mcmini/model/program.hpp"
#include "mcmini/model_checking/stats.hpp"
#include "mcmini/real_world/process.hpp"

namespace model_checking {

/**
 * @brief An abstraction representing a function which can formally verify
 * the correctness of a program modeled under McMini.
 */
class algorithm {
public:
  enum class exploration_policy : uint { round_robin, smallest_first };

  struct context {
    virtual const model::program &get_model() const = 0;
    virtual std::vector<const model::transition *>
    linearize_lowest_first() const = 0;
    virtual std::vector<const model::transition *>
    linearize_optimal() const = 0;
    std::vector<const model::transition *> linearize_trace(bool optimal) const {
      return optimal ? linearize_optimal() : linearize_lowest_first();
    }
  };

  struct callbacks {
    virtual void crash(const context &, const stats &) const {}
    virtual void deadlock(const context &, const stats &) const {}
    virtual void data_race(const context &, const stats &) const {}
    virtual void trace_completed(const context &, const stats &) const {}

    virtual void
    abnormal_termination(const context &, const stats &,
                         const real_world::process::termination_error &) const {
    }
    virtual void nonzero_exit_code(
        const context &, const stats &,
        const real_world::process::nonzero_exit_code_error &) const {}

    virtual void
    undefined_behavior(const context &, const stats &,
                       const model::undefined_behavior_exception &) const {}
  };

  /**
   * @brief Run this model-checking algorithm using coordinator _coordinator_.
   *
   * To verify the correctness of a program using explicit-state model checking,
   * any verification algorithm must investigate all possible states of that
   * program. Each particular state of the program is represented in McMini by
   * an instance `model::state`. A state plus the collection of
   * transitions which are defined at that state is captured in a
   * `model::program`.
   *
   * The `model::program` conceptually represents the "history" of
   * changes that some process underwent. The process whose states are described
   * by the program is represented `real_world::process`.
   *
   * @param coordinator the coordinator which manages the syncrhonization of the
   * program and the processes which those programs represent.
   * @param callbacks a set of functions which are invoked as verification takes
   * place. The callbacks will be invoked when the algorithm encounters the
   * following violations:
   *
   * - when a state _s_ is encountered that is in _deadlock_.(i.e. there is no
   * way to make any more progress). Formally, this means there are no enabled
   * transitions at state _s_.
   * - when a state _s_ has a data race.
   * - if, at any point during verification, one of the corresponding processes
   * crashes.
   */
  virtual void verify_using(coordinator &coordinator,
                            const callbacks &callbacks) = 0;
  virtual ~algorithm() = default;

  void verify_using(coordinator &coordinator) {
    callbacks no_callbacks;
    this->verify_using(coordinator, no_callbacks);
  }
};

}; // namespace model_checking
