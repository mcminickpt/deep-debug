#include "mcmini/real_world/process/fork_process_source.hpp"

#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "mcmini/common/shm_config.h"
#include "mcmini/defines.h"
#include "mcmini/log/logger.hpp"
#include "mcmini/misc/extensions/unique_ptr.hpp"
#include "mcmini/real_world/mailbox/runner_mailbox.h"
#include "mcmini/real_world/process.hpp"
#include "mcmini/real_world/process/local_linux_process.hpp"
#include "mcmini/real_world/process/multithreaded_fork_process_source.hpp"
#include "mcmini/real_world/process/resources.hpp"
#include "mcmini/real_world/process/template_process.h"
#include "mcmini/real_world/process_source.hpp"
#include "mcmini/real_world/shm.hpp"
#include "mcmini/signal.hpp"

using namespace logging;
using namespace real_world;
using namespace extensions;

logger fps("fork");
logger mfps("mtf");

fork_process_source::fork_process_source(real_world::target&& tp)
    : fork_process_source(tp) {}

fork_process_source::fork_process_source(
    const real_world::target& target_program)
    : template_program(extensions::make_unique<target>(target_program)) {
  this->template_program->set_preload_libmcmini();
}

multithreaded_fork_process_source::multithreaded_fork_process_source(
    const std::string& ckpt_file) {
  log_debug(mfps) << "Initialized mtf source with checkpoint file `"
                  << ckpt_file << "`";
  this->template_program = extensions::make_unique<dmtcp_target>(
      std::string("dmtcp_restart"),
      std::vector<std::string>{"--new-coordinator", "--port", "0"}, ckpt_file);
  this->template_program->set_env("MCMINI_NEEDS_STATE", "1");

  // Possibly a dmtcp program...

  // Here `libmcmini.so` doesn't need to be preloaded: it is assumed that
  // `mcmini` is contained in the checkpoint image that is restored by
  // `dmtcp_restart`. Hence we can omit
  // ```
  // this->target_program.set_preload_libmcmini();
  // ```
}

std::unique_ptr<process> fork_process_source::make_new_process() {
  shared_memory_region* rw_region =
      xpc_resources::get_instance().get_rw_region();

  // 1. Set up phase (LD_PRELOAD, binary sempahores, template process creation)
  xpc_resources::get_instance().reset_binary_semaphores_for_new_branch();
  if (!has_template_process_alive()) make_new_template_process();

  // 2. Check if the current template process has previously exited; if so, it
  // would have delivered a `SIGCHLD` to this process. By default this signal is
  // ignored, but McMini explicitly captures it (see `signal_tracker`).
  if (signal_tracker::instance().try_consume_signal(SIGCHLD)) {
    this->template_process_handle = nullptr;
    throw process_source::process_creation_exception(
        "Failed to create a new process (template process died). Consider "
        "using a previous checkpoint image");
  }

  // 3. If the current template process is alive, tell it to spawn a new
  // process and then wait for it to successfully call `fork(2)` to tell us
  // about its new child.
  const volatile template_process_t* tstruct =
      &(rw_region->as<mcmini_shm_file>()->tpt);

  signal_tracker::set_sem((sem_t*)&tstruct->mcmini_process_sem);
  if (sem_post((sem_t*)&tstruct->libmcmini_sem) != 0) {
    throw process_source::process_creation_exception(
        "The template process (" +
        std::to_string(this->template_process_handle->get_pid()) +
        ") was not synchronized with correctly: " +
        std::string(strerror(errno)));
  }
  int rc = signal_tracker::sig_semwait((sem_t*)&tstruct->mcmini_process_sem);
  if (rc != 0) {
    throw process_source::process_creation_exception(
        "The template process (" +
        std::to_string(this->template_process_handle->get_pid()) +
        ") was not synchronized with correctly: " +
        std::string(strerror(errno)));
  }

  if (signal_tracker::instance().try_consume_signal(SIGCHLD)) {
    // TODO: At this point, McMini may have multiple child processes.
    // Calling `prctl(PR_SETSUBREAPER)` in the branch processes means
    // that McMini receives a `SIGCHLD` when both the branch process and the
    // template process exit unexpectedly. If a `SIGCHLD` is delivered by
    // _either_ process, we would reach this error branch. To distinguish which
    // process exited unexpectedly under our feet, we would need to use
    // `waitpid(-1, &status, WNOHANG)` and check the status for _each_ child.
    this->template_process_handle = nullptr;
    throw process_source::process_creation_exception(
        "Failed to create a new process (template or [possibly the branch] "
        "process died)");
  }

  if (tstruct->cpid == TEMPLATE_FORK_FAILED) {
    throw process_source::process_creation_exception(
        "The `fork(2)` call in the template process failed unexpectedly "
        "(errno " +
        std::to_string(tstruct->err) + "): " + strerror(tstruct->err));
  }
  return extensions::make_unique<local_linux_process>(tstruct->cpid, false);
}

void fork_process_source::make_new_template_process() {
  // Reset first. If an exception is raised in subsequent steps, we don't want
  // to erroneously think that there is a template process when indeed there
  // isn't one.
  this->template_program->set_is_template();
  log_verbose(fps) << "Resetting template handle";
  this->template_process_handle = nullptr;
  log_verbose(fps) << "Template handle reset. Launching template program";
  this->template_process_handle = extensions::make_unique<local_linux_process>(
      this->template_program->launch_dont_wait(), false);
  log_verbose(fps) << "Template process `fork()`-ed";
}

void multithreaded_fork_process_source::make_new_template_process() {
  // Here we need, in addition, to wait for the template thread
  // to have heard back from all userspace threads before declaing the template
  // process is ready.
  shared_memory_region* rw_region =
      xpc_resources::get_instance().get_rw_region();
  const volatile template_process_t* tstruct =
      &(rw_region->as<mcmini_shm_file>()->tpt);

  signal_tracker::set_sem((sem_t*)&tstruct->mcmini_process_sem);

  fork_process_source::make_new_template_process();
  log_verbose(mfps) << "Waiting for the template thread to stabilize";

  // Wait for the restarted template to signal readiness -- but do NOT block
  // forever if it dies first. A failed or incompatible checkpoint (or any crash
  // during `dmtcp_restart`) can kill the template before it ever posts, and
  // SIGCHLD is not guaranteed to reach us (e.g. `dmtcp_restart` may reparent the
  // restored process, so the plain `sig_semwait` above would hang indefinitely
  // with no diagnostic). Instead, poll the readiness semaphore with a timeout
  // and, whenever it isn't yet posted, actively check that the template process
  // is still alive; if it died, fail fast with a clear error.
  const pid_t template_pid = this->template_process_handle->get_pid();
  sem_t* ready_sem = (sem_t*)&tstruct->mcmini_process_sem;
  while (true) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;  // re-check liveness at most ~1s after a death
    const bool acquired = sem_timedwait(ready_sem, &deadline) == 0;
    if (!acquired && errno == EINTR) continue;
    if (!acquired && errno != ETIMEDOUT) {
      throw process_source::process_creation_exception(
          "Error while waiting for the template process (" +
          std::to_string(template_pid) +
          ") to stabilize: " + std::string(strerror(errno)));
    }

    // Either the semaphore was posted or we timed out. In both cases confirm
    // the template is still alive before trusting a post: the readiness
    // semaphore is ALSO posted by the SIGCHLD handler on child death, so a
    // successful acquire does not by itself imply the template stabilized.
    // `WNOWAIT` peeks without reaping, leaving the child for its normal handle
    // destructor.
    siginfo_t info;
    info.si_pid = 0;  // lets us distinguish "no state change" under WNOHANG
    const bool template_died =
        waitid(P_PID, template_pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
        info.si_pid == template_pid;
    if (template_died) {
      this->template_process_handle = nullptr;
      throw process_source::process_creation_exception(
          "The template process (" + std::to_string(template_pid) +
          ") died during `dmtcp_restart` before signaling readiness. This "
          "usually means the checkpoint failed to restore (e.g. a corrupt or "
          "incompatible checkpoint image).");
    }
    if (acquired) return;  // template is alive and signaled readiness
    // Timed out but still alive: keep waiting.
  }
}
