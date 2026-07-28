#pragma once

#include "mcmini/defines.h"
#include "mcmini/misc/extensions/unique_ptr.hpp"
#include "mcmini/model/visible_object_state.hpp"

namespace model {
namespace objects {

struct mutex : public model::visible_object_state {
 public:
  /* The four possible states for a mutex */
  enum state { uninitialized, unlocked, locked, destroyed };

 private:
  state current_state = state::uninitialized;
  pthread_mutex_t* location;
  runner_id_t owner;

 public:
  mutex() = default;
  ~mutex() = default;
  mutex(const mutex &) = default;
  // `location` has no default member initializer (unlike, say,
  // condition_variable's `policy`), so it must always be passed explicitly
  // -- a mutex without its real address is meaningless, and every
  // mutex_lock/unlock faithfully forwards whatever get_location() returns,
  // so an uninitialized one corrupts every subsequent state derived from
  // it (see doc history around commit 9bd9ecf). `tid` defaults to
  // RID_INVALID: "unlocked, no owner" is exactly what callers that omit it
  // (mutex_init, condition_variable_enqueue_thread) mean.
  mutex(state s, pthread_mutex_t* loc, runner_id_t tid = RID_INVALID)
      : current_state(s), location(loc), owner(tid) {}
  
  // ---- State Observation --- //
  bool operator==(const mutex &other) const {
    return this->current_state == other.current_state && this->owner == other.owner;

  }
  bool operator!=(const mutex &other) const {
    return this->current_state != other.current_state;
  }
  bool is_locked_by(runner_id_t tid) const { return current_state == locked && owner ==tid; }

  bool is_locked() const { return this->current_state == locked; }
  bool is_unlocked() const { return this->current_state == unlocked; }
  bool is_destroyed() const { return this->current_state == destroyed; }
  bool is_initialized() const { return this->current_state != uninitialized; }

  
  pthread_mutex_t* get_location() const { return this->location; }

  std::unique_ptr<visible_object_state> clone() const override {
    return extensions::make_unique<mutex>(*this);
  }
  std::string to_string() const override {
    return "mutex(" + std::to_string(current_state) + ")";
  }
};
}  // namespace objects
}  // namespace model
