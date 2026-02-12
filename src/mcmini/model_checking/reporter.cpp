#include "mcmini/model_checking/reporter.hpp"

#include "mcmini/signal.hpp"

using namespace model;
using namespace model_checking;

void reporter::dump_relinearized_trace(std::ostream &os,
                                       const algorithm::context &c,
                                       const stats &) const {
  if (relinearize_traces) {
    os << "\nRELINEARIZED TRACE\n";
    for (const auto &t : c.linearize_trace(use_optimal_linearization)) {
      os << "thread " << t->get_executor() << ": " << t->to_string() << "\n";
    }
  }
}

void reporter::trace_completed(const algorithm::context &c,
                               const stats &stats) const {
  if (!verbose) return;
  std::cout << "TRACE " << stats.trace_id << "\n";
  for (const auto &t : c.get_model().get_trace()) {
    std::cout << "thread " << t->get_executor() << ": " << t->to_string()
              << "\n";
  }
  dump_relinearized_trace(std::cout, c, stats);
  std::cout << "\nNEXT THREAD OPERATIONS\n";
  for (const auto &tpair : c.get_model().get_pending_transitions()) {
    std::cout << "thread " << tpair.first << ": " << tpair.second->to_string()
              << "\n";
  }
  std::cout.flush();
}

void reporter::deadlock(const algorithm::context &c, const stats &stats) const {
  std::cout << "DEADLOCK (trace ID: " << stats.trace_id << ")" << std::endl;
  for (const auto &t : c.get_model().get_trace()) {
    std::cout << "thread " << t->get_executor() << ": " << t->to_string()
              << "\n";
  }
  dump_relinearized_trace(std::cout, c, stats);
  std::cout << "\nNEXT THREAD OPERATIONS\n";
  for (const auto &tpair : c.get_model().get_pending_transitions()) {
    std::cout << "thread " << tpair.first << ": " << tpair.second->to_string()
              << "\n";
  }
  std::cout.flush();
}

void reporter::undefined_behavior(
    const algorithm::context &c, const stats &stats,
    const undefined_behavior_exception &ub) const {
  std::cout << "UNDEFINED BEHAVIOR:\n" << ub.what() << std::endl;
  std::cout << "TRACE " << stats.trace_id << "\n";
  for (const auto &t : c.get_model().get_trace()) {
    std::cout << "thread " << t->get_executor() << ": " << t->to_string()
              << "\n";
  }
  dump_relinearized_trace(std::cout, c, stats);
  std::cout << "\nNEXT THREAD OPERATIONS\n";
  for (const auto &tpair : c.get_model().get_pending_transitions()) {
    std::cout << "thread " << tpair.first << ": " << tpair.second->to_string()
              << "\n";
  }
  std::cout.flush();
}

void reporter::abnormal_termination(
    const algorithm::context &c, const stats &stats,
    const real_world::process::termination_error &ub) const {
  std::cout << "Abnormally Termination (signo: " << ub.signo
            << ", signal: " << sig_to_str.at(ub.signo) << "):\n"
            << ub.what() << std::endl;

  std::cout << "TRACE " << stats.trace_id << "\n";
  for (const auto &t : c.get_model().get_trace()) {
    std::cout << "thread " << t->get_executor() << ": " << t->to_string()
              << "\n";
  }
  dump_relinearized_trace(std::cout, c, stats);
  const transition *terminator =
      c.get_model().get_pending_transition_for(ub.culprit);
  std::cout << "thread " << terminator->get_executor() << ": "
            << terminator->to_string() << "\n";

  std::cout << "\nNEXT THREAD OPERATIONS\n";
  for (const auto &tpair : c.get_model().get_pending_transitions()) {
    if (tpair.first == terminator->get_executor()) {
      std::cout << "thread " << tpair.first << ": executing"
                << "\n";
    } else {
      std::cout << "thread " << tpair.first << ": " << tpair.second->to_string()
                << "\n";
    }
  }
  std::cout << stats.total_transitions + 1 << " total transitions executed"
            << std::endl;
}
