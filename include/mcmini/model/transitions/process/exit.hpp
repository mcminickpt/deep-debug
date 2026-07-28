#pragma once

#include "mcmini/model/objects/thread.hpp"
#include "mcmini/model/transition.hpp"

namespace model {
namespace transitions {

struct process_exit : public model::transition {
 public:
  int exit_code = -1;
  process_exit(state::runner_id_t executor) : process_exit(executor, -1) {}
  process_exit(state::runner_id_t executor, int exit_code)
      : transition(executor), exit_code(exit_code) {}
  ~process_exit() = default;

  status modify(model::mutable_state& s) const override {
    // Mark the executor exited, like thread_exit does (minus its
    // RID_MAIN_THREAD restriction, since exit()/abort() can end the process
    // from any thread). Without this, is_active() keeps reporting true, so
    // classic_dpor re-selects this same "enabled" transition forever for any
    // exit code its program_exit_code() > 0 check doesn't already catch
    // (i.e. exit code 0).
    using namespace model::objects;
    s.add_state_for_runner(executor, new thread(thread::exited));
    return status::exists;
  }

  int program_exit_code() const override { return exit_code; }

  std::string to_string() const override { return "exit(2) (syscall)"; }
};

}  // namespace transitions
}  // namespace model
