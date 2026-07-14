#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "mcmini/defines.h"
#include "mcmini/log/severity_level.hpp"

namespace model {

struct config {
  /**
   * The maximum number of transitions that can be run
   * by any _single thread_ while running the model checker
   */
  uint64_t max_thread_execution_depth;

  /**
   * The maximum number of transitions that can be contained in any given trace.
   */
  uint64_t maximum_total_execution_depth = 1500;

  /**
   * The trace id to stop the model checker at
   * to print the contents of the transition stack
   */
  trid_t target_trace_id;

  /**
   * Whether or not exploration occurs using round robin scheduling (default is
   * smallest tid first)
   */
  bool use_round_robin_scheduling = false;

  /**
   * Whether model checking should halt at the first encountered deadlock
   */
  bool stop_at_first_deadlock = false;

  /**
   * Whether relinearization should occur before displying buggy traces.
   *
   * A relinearization `T'` of a trace `T` is a permutation `sigma_n` such that
   * `happens_before_T(i, j) <--> happens_before_T'(sigma(i), sigma(j))`.
   * Informally, it is a reordering of the transitions in the trace that
   * produces the same final state (and hence the same bugs, crashes, etc).
   *
   * The interest in relinearization is that although round-robin exploration in
   * theory should enable McMini to capture shallower bugs more readily, the
   * tradeoff is less readable traces when they're produced because the user
   * will frequently have to switch between different threads to analyze the
   * trace. Frequent switching makes it challenging to have a global idea of the
   * issue causing a bug, even for small traces.
   *
   * With this enabled, McMini will first reorder the transitions using a greedy
   * approach as follows:
   *
   * 1. When given the choice, McMini will first choose transitions of the same
   * thread ID as the thread ID of the last chosen transition.
   * 2. When McMini is forced by the happens-before relation to order a
   * transition of a different thread ID, among the possible choices it chooses
   * the smallest.
   */
  bool relinearize_traces = false;

  /**
   * Whether or not traces are relinearized optimally. Implies
   * `relinearize_traces`.
   *
   * By default, relinearizations are created using a greedy algorithm by
   * grouping together, as long as possible, transitions of the same thread ID
   * subject to the happens-before relation over them. However, a
   * relinearization produced using the greedy method is not guaranteed to
   * minimize the number of "context switches" a user has to make. That is,
   * there may exist a better linearization obeying the same happens-before
   * relation but with a fewer number of changes between thread IDs.
   *
   * The optimization problem can be formulated as follows:
   *
   * Consider a colored DAG, G, with vertices V and edges E and a coloring
   * function f: V —> C (where C is a finite set of colors). A linearization L
   * of the vertices of G is a sequence of vertices v_I such
   * that the order of v_I obeys the dag: a node ‘u’ appears before any node
   * with which a directed edge from that node to any other node (formally u < v
   * in L iff (u,v) in E).
   *
   * A colored linearization is the sequence of colors c_1, c_2, … of a
   * linearization v_1, v_2, …, where c_i = f(v_i). Let h: L —> N
   * denote the number of “inversions” in a linearization L.
   * A linearization L has an inversion (v_i, v_i+1) iff f(v_i) != f(v_i+1). Let
   * Inv: L -> Nat denote the number of inversions in a linearization L. Then
   * MinInversions is the problem
   *
   * min (Inv L) where L is a linearization of G.
   *
   * The problem can be modeled as an instance of the Sequential Ordering
   * Problem. Since in general this problem is challenging to solve, linearizing
   * large traces may become impractical.
   */
  bool use_optimal_linearization = false;

  /**
   * Informs McMini that the target executable should be run under DMTCP with
   * `libmcmini.so` configured in record mode.
   */
  bool record_target_executable_only = false;

  /**
   * Informs McMini that model checking with the checkpoint file should occur
   * using `multithreaded_fork()` + a template process instead of
   * `dmtcp_restart` to create new branches
   */
  bool use_multithreaded_fork = false;

  /**
   * Whether to print every trace, or one those that cause a bug
   */
  bool verbose = false;

  /**
   * Whether the STDOUT/STDERR of the program is sent to /dev/null during
   * execution
   */
  bool quiet_program_output = false;

  /**
   * The time between consecutive checkpoint images when `libmcmini.so` is
   * running in record mode.
   */
  std::chrono::seconds checkpoint_period;

  /**
   * The path to the checkpoint file that should be used to begin deep debugging
   * from.
   */
  std::string checkpoint_file = "";

  // Name of the target executable that will be model checked
  std::string target_executable = "";

  // A list of arguments to be passed to the executable on launch
  std::vector<std::string> target_executable_args;

  // Default severity level for logging. Overridden if a blacklist/whitelist
  // file path is provided (TODO).
  logging::severity_level global_severity_level = logging::severity_level::info;
};
} // namespace model
