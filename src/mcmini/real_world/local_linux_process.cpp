#include "mcmini/real_world/process/local_linux_process.hpp"

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include <atomic>
#include <cstring>
#include <iostream>

#include "mcmini/common/shm_config.h"
#include "mcmini/log/logger.hpp"
#include "mcmini/real_world/mailbox/runner_mailbox.h"
#include "mcmini/real_world/process/fork_process_source.hpp"
#include "mcmini/real_world/process/resources.hpp"
#include "mcmini/real_world/shm.hpp"
#include "mcmini/signal.hpp"

using namespace logging;
using namespace real_world;

logger process_logger("processes");

local_linux_process::local_linux_process(pid_t pid, bool should_wait)
    : pid(pid), should_wait(should_wait) {}

local_linux_process::local_linux_process(local_linux_process &&other)
    : local_linux_process(other.pid) {
  other.pid = -1;
}

local_linux_process &local_linux_process::operator=(
    local_linux_process &&other) {
  this->pid = other.pid;
  other.pid = -1;
  return *this;
}

local_linux_process::~local_linux_process() {
  if (pid <= 0) {
    return;
  }
  if (kill(pid, SIGUSR1) == -1) {
    log_error(process_logger)
        << "Error sending SIGUSR1 to `" << (pid) << "`: " << strerror(errno);
  }
  int status;
  if (should_wait) {
    if (waitpid(pid, &status, 0) == -1) {
      if (errno != ECHILD) {
        log_error(process_logger) << "Error waiting for process (waitpid) `"
                                  << pid << "`: " << strerror(errno);
      } else {
        log_error(process_logger) << "Error: " << strerror(errno);
      }
    } else {
      // This death is expected and already fully handled (we just reaped
      // it above): consume the SIGCHLD it generated, so it doesn't linger in
      // signal_tracker's counter. Otherwise the next process this class
      // creates (see coordinator::return_to_depth()/assign_new_process_handle(),
      // which destroys the old handle immediately before creating a new
      // one) would see that leftover count on its own first execute_runner()
      // call and wrongly conclude *it* had just died, even though it's
      // still alive and simply hasn't responded yet.
      signal_tracker::instance().try_consume_signal(SIGCHLD);

      // The process we just killed may itself have left behind other
      // now-orphaned descendants (e.g. its own private DMTCP coordinator,
      // spawned by `dmtcp_restart --new-coordinator`) that die/reparent to
      // us -- as the PR_SET_CHILD_SUBREAPER subreaper -- around the same
      // time, generating their own SIGCHLDs. signal_tracker's counter isn't
      // tied to a specific pid, so drain every zombie that's already
      // reapable right now (non-blocking: this must never wait on a
      // descendant that hasn't died yet) and consume one signal per reap,
      // so none of them linger to be misattributed later either.
      while (waitpid(-1, &status, WNOHANG) > 0) {
        signal_tracker::instance().try_consume_signal(SIGCHLD);
      }
    }
  }
}

volatile runner_mailbox *local_linux_process::execute_runner(runner_id_t id) {
  shared_memory_region *shm_slice =
      xpc_resources::get_instance().get_rw_region();
  volatile runner_mailbox *rmb =
      &(shm_slice->as_array_of<mcmini_shm_file>()->mailboxes[id]);

  signal_tracker::set_sem((sem_t *)&rmb->model_side_sem);

  // NOTE: At the moment, each process has the entire view of the
  // shared memory region at its disposal. If desired, an extra layer could be
  // added on top which manages allocating slices of a `shared_memory_region`
  // and "allocates" them to different processes. This would look similar to
  // `malloc()/free()`, where the `free()` would be triggered by the destructor
  // of the slice. This is overly complicated at the moment, and we simply
  // restrict the number of proxy processes to one.
  mc_wake_thread(rmb);

  // There is a potential race if the child dies and issues a SIGCHLD
  // just before we call `mc_wait_for_thread()`. This doesn't happen
  // because the signal handler for SIGCHLD will call `mc_wake_scheduler()`
  // which issues a `sem_post(3)` to cancel the `sem_wait(3)` in
  // `mc_wait_for_thread()`. The signal handler also sets `sigchld_set`,
  // allowing us to determine that a SIGCHLD was received.
  //
  // TODO: The template process will also send a SIGCHLD if it dies
  // unexpectedly. Because we don't expect the template process to die, this is
  // OK for now, but should be handled in the future.
  while (true) {
    errno = 0;
    signal_tracker::sig_semwait((sem_t *)&rmb->model_side_sem);
    if (!signal_tracker::instance().try_consume_signal(SIGCHLD)) {
      break;
    }
    // signal_tracker's SIGCHLD count is process-wide, not tied to a pid, so
    // a pending count doesn't necessarily mean *this->pid* is the one that
    // died -- some other descendant (e.g. a previous branch's own private
    // DMTCP coordinator) may have generated it instead. Confirm with a
    // non-blocking waitpid() specifically on this->pid (PR_SET_CHILD_SUBREAPER,
    // set in target::prepare_mcmini_targets(), lets us wait on it even though
    // it isn't a direct child): if it hasn't actually changed state, this
    // SIGCHLD wasn't ours, so just resume waiting instead of misreporting a
    // termination that didn't happen.
    int status;
    int rc = waitpid(this->pid, &status, WNOHANG);
    if (rc == 0) {
      continue;
    }
    if (rc == -1) {
      throw process::execution_error(
          "Error attempting to determine the failure causing the child "
          "process to abnormally exit (or possibly an internal error of "
          "McMini): " + std::string(strerror(errno)));
    } else if (WIFEXITED(status)) {
      const int exit_code = WEXITSTATUS(status);
      if (exit_code != 0) {
        throw process::nonzero_exit_code_error(
            exit_code, id, "The program exited with code " + std::to_string(exit_code));
      }
      // A clean (code 0) exit reaching this SIGCHLD-based detection path is
      // not the same situation nonzero_exit_code_error reports: that
      // exception means the *target program* exited abnormally, a bug for
      // the user to fix. Here, the whole process fully terminated on its
      // own while this runner still had a transition pending on it -- i.e.
      // it bypassed the model-driven exit protocol (mc_transparent_exit(),
      // fixed in commit 6bed56a, deliberately keeps the process alive
      // across both of its mailbox rounds before ever calling the real
      // exit(2)) rather than checking in normally. That is a McMini-side
      // protocol violation to investigate, not a target-program bug.
      throw process::execution_error(
          "Runner " + std::to_string(id) +
          "'s process exited normally (code 0) while a transition was "
          "still pending on it, bypassing the model-driven exit protocol.");
    } else if (WIFSIGNALED(status)) {
      throw process::termination_error(
          WTERMSIG(status), id,
          "Process terminated abnormally by signal " +
              std::to_string(WTERMSIG(status)));
    } else {
      throw process::execution_error(
          "SIGSTOP/SIGCONT in branch processes is not yet supported.");
    }
  }
  return rmb;
}
