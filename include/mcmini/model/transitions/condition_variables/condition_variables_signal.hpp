#pragma once

#include "mcmini/model/objects/condition_variables.hpp"
#include "mcmini/model/transition.hpp"
#include "mcmini/model/transitions/condition_variables/condition_variables_wait.hpp"
#include "mcmini/model/transitions/mutex/mutex_unlock.hpp"

namespace model {
namespace transitions {

struct condition_variable_signal : public model::transition {
 private:
  const state::objid_t cond_id; /* The condition variable this transition signals */
  mutable bool had_waiters = false;
  state::objid_t get_objid_by_location(const mutable_state& s, pthread_mutex_t* mutex_location) const {
    
    for (state::objid_t id = 0; id < invalid_objid; id++){
      if (!s.contains_object_with_id(id)) continue;

      // const visible_object_state* obj = s.state::get_state_of_object(id);
      const visible_object_state* obj = s.get_state_of_object<visible_object_state>(id);
      const objects::mutex* m = dynamic_cast<const objects::mutex*>(obj);
      if (m && m->get_location() == mutex_location) {
        return id;
      }
    }
    return invalid_objid;
  }

 public:
  condition_variable_signal(runner_id_t executor, state::objid_t cond_id)
    : transition(executor), cond_id(cond_id) {}
  ~condition_variable_signal() = default;

  status modify(model::mutable_state& s) const override {
    using namespace model::objects;

    // Retrive the state of the condition variable
    const condition_variable* cv = s.get_state_of_object<condition_variable>(cond_id);

    // Count waiting threads BEFORE signal
    int prev_waiting_count = 0;
    for (auto id: cv->get_policy()->return_wait_queue()) {
      if (cv->get_policy()->get_thread_cv_state(id) == CV_WAITING) {
        prev_waiting_count++;
      }
    }

    if (cv->is_uninitialized()) {
      return status::undefined;
    }
    
    if (cv->is_destroyed()) {
      return status::undefined;
    }
    
    // Check if there are waiters (if not, signal is a no-op but still valid)
    if (!cv->has_waiters()) {
      return status::exists; //valid transition (lost wakeup)
    }

    if (!cv->has_waiters()) {
      return status::exists; // valid transition (lost wakeup)
    }
  
    // Find only CV_WAITING threads (not CV_PREWAITING)
    std::vector<runner_id_t> waiting_threads;
    const auto& wait_queue = cv->get_policy()->return_wait_queue();
    for (auto tid : wait_queue) {
      if (cv->get_policy()->get_thread_cv_state(tid) == CV_WAITING) {
        waiting_threads.push_back(tid);
      }
    }

    // Clone the policy rather than mutating cv->get_policy() in place -- see
    // condition_variable_enqueue_thread::modify()'s identical comment for
    // why: is_enabled_in() runs this same modify() against a throwaway
    // diff_state just to check the status, and an in-place mutation on the
    // old, shared policy would escape that throwaway diff and permanently
    // corrupt the committed state.
    ConditionVariablePolicy* new_policy = cv->get_policy()->clone();
    // Add only CV_WAITING threads to wake groups
    if (!waiting_threads.empty()) {
      new_policy->add_to_wake_groups(waiting_threads);
    }

    // Update the condition variable state
    const int new_waiting_count = new_policy->return_wait_queue().size();
    condition_variable::state new_state = new_waiting_count > 0
                                          ? condition_variable::cv_waiting
                                          : condition_variable::cv_signaled;
    // Also preserve the mutex association: it isn't passed through the
    // constructor here, so it would otherwise be left uninitialized, making
    // condition_variable_wait::modify()'s `m->get_location() ==
    // cv->get_mutex()` check fail forever afterward.
    condition_variable* new_cv = new condition_variable(new_state, RID_INVALID, nullptr, new_waiting_count);
    new_cv->set_policy(new_policy);
    new_cv->set_associated_mutex(cv->get_mutex());
    s.add_state_for_obj(cond_id, new_cv);
    condition_variable mutable_cv(new_state, RID_INVALID, nullptr, new_waiting_count);
    mutable_cv.check_for_lost_wakeup(true, prev_waiting_count); // Check for lost wakeup if this was a signal

    return status::exists;   
  }

  state::objid_t get_id() const { return this->cond_id; }
  std::string to_string() const override {
    return "pthread_cond_signal(cond:" + std::to_string(cond_id) + ")";
  }
  // MARK: Model checking functions
  bool depends(const condition_variable_wait* cw) const {
    return this->cond_id == cw->get_id();
  }
  bool depends(const mutex_lock* ml) const {
    return this->cond_id == ml->get_id();
  }
  bool coenabled_with(const mutex_unlock* mu) const {
    return this->cond_id != mu->get_id();
  }
  bool coenabled_with(const condition_variable_wait* cw) const {
    return this->cond_id != cw->get_id();
  }

};
} // namespace transitions
} // namespace model
