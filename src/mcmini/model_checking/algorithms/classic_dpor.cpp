#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <list>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef MCMINI_USE_SCIP
#include <numeric>

#include "scip/scip.h"
#include "scip/scipdefplugins.h"
#endif

#include "mcmini/coordinator/coordinator.hpp"
#include "mcmini/defines.h"
#include "mcmini/log/logger.hpp"
#include "mcmini/model/config.hpp"
#include "mcmini/model/exception.hpp"
#include "mcmini/model/program.hpp"
#include "mcmini/model/transition.hpp"
#include "mcmini/model/transitions/condition_variables/callbacks.hpp"
#include "mcmini/model/transitions/mutex/callbacks.hpp"
#include "mcmini/model/transitions/mutex/mutex_init.hpp"
#include "mcmini/model/transitions/semaphore/callbacks.hpp"
#include "mcmini/model/transitions/thread/callbacks.hpp"
#include "mcmini/model_checking/algorithms/classic_dpor.hpp"
#include "mcmini/model_checking/algorithms/classic_dpor/clock_vector.hpp"
#include "mcmini/model_checking/algorithms/classic_dpor/runner_item.hpp"
#include "mcmini/model_checking/algorithms/classic_dpor/stack_item.hpp"
#include "mcmini/real_world/process.hpp"

using namespace model;
using namespace logging;
using namespace model_checking;

logger dpor_logger("dpor");

struct classic_dpor::dpor_context : public algorithm::context {
 public:
  ::coordinator &coordinator;
  std::vector<model_checking::stack_item> stack;

  dpor_context(::coordinator &c) : coordinator(c) {}

 public:
  const size_t state_stack_size() const { return stack.size(); }
  const size_t transition_stack_size() const {
    const size_t ss = state_stack_size();
    return ss > 0 ? (ss - 1) : 0;
  }

  const transition *get_transition(int i) const {
    return stack.at(i).get_out_transition();
  }

  const transition *get_transition(size_t i) const {
    return stack.at(i).get_out_transition();
  }

  const runner_id_t tid(size_t i) const {
    return get_transition(i)->get_executor();
  }

  const clock_vector &clock(size_t j) const {
    // C(j) in the DPOR paper
    // NOTE: The clock vector for the `i`th transition is stored in the state
    // into which the transition points, NOT the state from which the transition
    // starts (hence the j + 1)
    return stack.at(j + 1).get_clock_vector();
  }

  bool happens_before(size_t i, size_t j) const {
    const runner_id_t procSi = get_transition(i)->get_executor();
    return i <= clock(j).value_for(procSi);
  }

  bool happens_before_thread(size_t i, runner_id_t p) const {
    const runner_id_t rid = get_transition(i)->get_executor();
    const clock_vector &cv =
        stack.back().get_per_runner_clocks()[p].get_clock_vector();
    return i <= cv.value_for(rid);
  }

  bool threads_race_after(size_t i, runner_id_t q, runner_id_t p) const {
    const size_t transition_stack_height = stack.size() - 1;
    for (size_t j = i + 1; j < transition_stack_height; j++) {
      if (q == get_transition(j)->get_executor() &&
          this->happens_before_thread(j, p))
        return true;
    }
    return false;
  }
  std::vector<std::vector<size_t>> transitive_reduction() const;
  std::vector<const transition *> linearize_lowest_first() const;
  std::vector<const transition *> linearize_optimal() const;
  const program &get_model() const {
    return this->coordinator.get_current_program_model();
  }
};

clock_vector classic_dpor::accumulate_max_clock_vector_against(
    const model::transition &t, const dpor_context &context) const {
  // The last state in the stack does NOT have an out transition, hence the
  // `nullptr` check. Note that `s_i.get_out_transition()` refers to `S_i`
  // (case-sensitive) in the paper, viz. the transition between states `s_i` and
  // `s_{i+1}`.
  clock_vector result;
  for (size_t i = 0; i < context.transition_stack_size(); i++) {
    const model::transition *S_i = context.get_transition(i);
    if (this->are_dependent(*S_i, t))
      result = clock_vector::max(result, context.clock(i));
  }
  return result;
}

bool classic_dpor::are_coenabled(const model::transition &t1,
                                 const model::transition &t2) const {
  return t1.get_executor() != t2.get_executor() &&
         this->config.coenabled_relation.call_or(true, &t1, &t2);
}

bool classic_dpor::are_dependent(const model::transition &t1,
                                 const model::transition &t2) const {
  return t1.get_executor() == t2.get_executor() ||
         this->config.dependency_relation.call_or(true, &t1, &t2);
}

void classic_dpor::verify_using(coordinator &coordinator,
                                const callbacks &callbacks) {
  // The code below is an implementation of the model-checking algorithm of
  // Flanagan and Godefroid from 2005.

  // 1. Data structure set up

  /// @invariant: The number of items in the DPOR-specific stack is the same
  /// size as the number of transitions in the current trace plus one.
  ///
  /// The initial entry into the stack represents the information DPOR tracks
  /// for state `s_0`.
  log_debug(dpor_logger) << "Initializing the DPOR stack";
  bool reached_max_depth = false;
  std::list<runner_id_t> round_robin_sched;

  stats model_checking_stats;
  dpor_context context(coordinator);
  auto &dpor_stack = context.stack;
  dpor_stack.emplace_back(
      clock_vector(),
      coordinator.get_current_program_model().get_enabled_runners());
  log_very_verbose(dpor_logger)
      << "Initial enabled runners: "
      << coordinator.get_current_program_model().get_enabled_runners();

  while (!dpor_stack.empty()) {
    // 2. Exploration phase
    while (dpor_stack.back().has_enabled_runners()) {
      if (dpor_stack.size() >= this->config.maximum_total_execution_depth) {
        if (!reached_max_depth) {
          log_unexpected(dpor_logger)
              << "*** Execution Limit Reached! ***\n\n"
              << "McMini encountered a trace with " << dpor_stack.size()
              << " transitions which is the most that McMini was configured"
              << " to handle in any given trace ("
              << this->config.maximum_total_execution_depth
              << "). McMini will continue its search, but it may be "
                 "incomplete. Rerun McMini with the "
                 "\"--max-depth-per-thread\" "
              << "flag for correct results.";
          reached_max_depth = true;
        }
        break;
      }
      try {
        runner_id_t rid;
        switch (config.policy) {
          case algorithm::exploration_policy::smallest_first: {
            rid = dpor_stack.back().get_first_enabled_runner();
            break;
          }
          case algorithm::exploration_policy::round_robin: {
            auto unscheduled_runners = dpor_stack.back().get_enabled_runners();

            // For round robin scheduling, always prefer to schedule
            // threads that don't appear in `round_robin_sched` because
            // these threads are necessarily unscheduled wrt the previous
            // starting point.
            //
            // 1. Among those threads not appearing in the round robin
            // schedule, always pick the smallest.
            // 2. If every enabled thread has already been scheduled, then pick
            // the first based on the round robin schedule and move it to the
            // back of the list. NOTE: Every enabled runner is in
            // `round_robin_sched`, but the converse is not necessarily true
            // (i.e. there may be threads in `round_robin_sched` that aren't
            // enabled)
            for (const runner_id_t id : round_robin_sched) {
              unscheduled_runners.erase(id);
            }
            if (!unscheduled_runners.empty()) {
              rid = *std::min_element(unscheduled_runners.begin(),
                                      unscheduled_runners.end());
              round_robin_sched.push_back(rid);
            } else {
              auto it = std::find_if(round_robin_sched.begin(),
                                     round_robin_sched.end(),
                                     [&](const runner_id_t id) {
                                       return dpor_stack.back().is_enabled(id);
                                     });
              assert(it != round_robin_sched.end());
              round_robin_sched.splice(round_robin_sched.end(),
                                       round_robin_sched, it);
              rid = *it;
            }
          }
        }

        this->continue_dpor_by_expanding_trace_with(rid, context);
        model_checking_stats.total_transitions++;

        // Now ask the question: will the next operation of this thread
        // cause the program to exit or abort abnormally?
        //
        // If so, stop expanding the trace and backtrack. The rationale is that
        // any extension of this trace will eventually show the same bug anyway
        // (the transition claims to cause the program to abort), and smaller
        // traces are more understandable.
        const transition *rtransition =
            coordinator.get_current_program_model().get_pending_transition_for(
                rid);
        if (rtransition->aborts_program_execution()) {
          throw real_world::process::termination_error(SIGABRT, rid,
                                                       "The program aborted");
        } else if (rtransition->program_exit_code() > 0) {
          throw real_world::process::nonzero_exit_code_error(
              rtransition->program_exit_code(), rid,
              "The program exited abnormally");
        }
      } catch (const model::undefined_behavior_exception &ube) {
        callbacks.undefined_behavior(context, model_checking_stats, ube);
        return;
      } catch (const real_world::process::termination_error &te) {
        callbacks.abnormal_termination(context, model_checking_stats, te);
        return;
      } catch (const real_world::process::nonzero_exit_code_error &nzec) {
        callbacks.nonzero_exit_code(context, model_checking_stats, nzec);
        return;
      }
    }

    callbacks.trace_completed(context, model_checking_stats);

    if (coordinator.get_current_program_model().is_in_deadlock()) {
      if (config.stop_at_first_deadlock) {
        log_info(dpor_logger)
            << "First deadlock found. Reporting and stopping model checking.";
        callbacks.deadlock(context, model_checking_stats);
        return;
      } else {
        callbacks.deadlock(context, model_checking_stats);
      }
    }

    // 3. Backtrack phase
    while (!dpor_stack.empty() && dpor_stack.back().backtrack_set_empty())
      dpor_stack.pop_back();

    if (!dpor_stack.empty()) {
      // At this point, the model checker's data structures are valid for
      // `dpor_stack.size()` states; however, the model and the associated
      // process(es) that the model represent do not yet correspond after
      // backtracking.
      log_debug(dpor_logger)
          << "Backtracking to depth `" << (dpor_stack.size() - 1) << "`";
      coordinator.return_to_depth(dpor_stack.size() - 1);
      log_debug(dpor_logger) << "Finished backtracking to depth `"
                             << (dpor_stack.size() - 1) << "`";
      model_checking_stats.trace_id++;

      // The first step of the NEXT exploration phase begins with following
      // one of the backtrack threads. Select one thread to backtrack upon and
      // follow it before continuing onto the exploration phase.
      try {
        this->continue_dpor_by_expanding_trace_with(
            dpor_stack.back().backtrack_set_pop_first(), context);

        // If we're doing round robin scheduling for expanding the trace,
        // backtracking forces a restart of the round robin process.
        // Intuitively, this makes sense: round robin only applies when
        // searching _new_ traces. However, we could be a little more
        // intelligent here and resume the round robin from where it _should_
        // continue, but that's a little more complicated.
        //
        // That is, if the trace after backtracking is e.g.
        //
        // 1, 2, 3, 1, 2, _2_
        //
        // where _2_ was inserted as a backtrack point, then we could notice
        // that _3_ was technically supposed to be next in the round robin
        // scheduling. The current method would schedule 1, then 2, then 3, ...
        // since we only account for scheduling that occurs during _expansion_.
        round_robin_sched.clear();
      } catch (const model::undefined_behavior_exception &ube) {
        callbacks.undefined_behavior(context, model_checking_stats, ube);
        return;
      }
    }
  }
}

void classic_dpor::continue_dpor_by_expanding_trace_with(
    runner_id_t p, dpor_context &context) {
  log_debug(dpor_logger) << "DPOR selected `" << p << "` to explore";
  context.coordinator.execute_runner(p);
  log_debug(dpor_logger) << "DPOR expanded following `" << p << "`";
  this->grow_stack_after_running(context);
  this->dynamically_update_backtrack_sets(context);
}

void classic_dpor::grow_stack_after_running(dpor_context &context) {
  // In this method, the following invariants are assumed to hold:
  //
  // 1. `n` := `stack.size()`.
  // 2. `t_n` is the `n`th transition executed in the model of `coordinator`.
  // This transition *has already executed in the model.* THIS IS VERY
  // IMPORTANT as the DPOR information tracking below relies on this heavily.
  //
  // After this method is executed, the stack will have size `n + 1`. Each entry
  // will correspond to the information DPOR cares about for each state in the
  // `coordinator`'s state sequence.
  const coordinator &coordinator = context.coordinator;
  assert(coordinator.get_depth_into_program() == context.stack.size());
  const model::transition *t_n =
      coordinator.get_current_program_model().get_trace().back();

  // NOTE: `cv` corresponds to line 14.3 of figure 4 in the DPOR paper.
  clock_vector cv = accumulate_max_clock_vector_against(*t_n, context);

  // NOTE: The assignment corresponds to line 14.4 of figure 4. Here `S'`
  // represents the transition sequence _after_ `t_n` has executed. Since the
  // `coordinator` already contains this transition (recall the invariants in
  // the comment at the start of this function), `S' ==
  // `coordinator.get_current_program_model().get_trace()` and thus `|S'| ==
  // corodinator.get_depth_into_program().
  cv[t_n->get_executor()] = coordinator.get_depth_into_program();

  // NOTE: This line corresponds to line 14.5 of figure 4. Here, C' is
  // conceptually captured through the DPOR stack and the per-thread DPOR data.
  // The former contains the per-state clock vectors while the latter the
  // per-thread clock vectors (among other data).
  auto s_n_per_runner_clocks = context.stack.back().get_per_runner_clocks();
  s_n_per_runner_clocks[t_n->get_executor()].set_clock_vector(cv);
  context.stack.emplace_back(
      cv, std::move(s_n_per_runner_clocks), t_n,
      coordinator.get_current_program_model().get_enabled_runners());
  stack_item &s_n = context.stack.at(context.stack.size() - 2);
  stack_item &s_n_plus_1 = context.stack.back();

  // INVARIANT: For each thread `p`, if such a thread is contained
  // in the sleep set of `s_n`, then `next(s_n, p)` MUST be the transition
  // that would be contained in that sleep set.
  for (const runner_id_t &rid : s_n.get_sleep_set()) {
    const model::transition *rid_next =
        coordinator.get_current_program_model().get_pending_transition_for(rid);
    if (this->are_independent(*rid_next, *t_n))
      s_n_plus_1.insert_into_sleep_set(rid);
  }

  // `t_n` is inserted into the sleep set AFTER execution. This is how sleep
  // sets work (see papers etc.)
  s_n.set_out_transition(t_n);
  s_n.insert_into_sleep_set(t_n->get_executor());
  s_n.mark_searched(t_n->get_executor());
}

void classic_dpor::dynamically_update_backtrack_sets(dpor_context &context) {
  /*
   * Updating the backtrack sets is accomplished as follows
   * (under the given assumptions)
   *
   * ASSUMPTIONS
   *
   * 1. The state reflects last(S) for the transition stack
   *
   * 2. The thread that ran last is at the top of the transition
   * stack (this should always be true)
   *
   * 3. The next transition for the thread that ran the most
   * recent transition in the transition stack (the transition at the
   * top of the stack) has been properly updated to reflect what that
   * thread will do next
   *
   * WLOG, assume there are `n` transitions in the transition stack
   * and `k` threads that are known to exist at the time of updating
   * the backtrack sets. Note this implies that there are `n+1` items
   * in the state stack (since there is always the initial state + 1
   * for every subsequent transition thereafter)
   *
   * Let
   *  S_i = ith backtracking state item
   *  T_i = ith transition
   *  N_p = the next transition for thread p (next(s, p))
   *
   * ALGORITHM:
   *
   * 1. First, get a reference to the transition at the top
   * of the transition stack (i.e. the most recent transition)
   * as well as the thread that ran that transition. WLOG suppose that
   * thread has a thread id `i`.
   *
   * This transition will be used to test against the transitions
   * queued as running "next" for all of the **other** threads
   * that exist
   *
   *  2. Test whether a backtrack point is needed at state
   *  S_n for the other threads by comparing N_p, for all p != i.
   *
   *  3. Get a reference to N_i and traverse the transition stack
   *  to determine if a backtrack point is needed anywhere for
   *  thread `i`
   */
  const coordinator &coordinator = context.coordinator;
  const size_t num_threads =
      coordinator.get_current_program_model().get_num_runners();

  std::set<runner_id_t> thread_ids;
  for (runner_id_t i = 0; i < num_threads; i++) thread_ids.insert(i);

  const ssize_t t_stack_top = (ssize_t)(context.stack.size()) - 2;
  const runner_id_t last_runner_to_execute =
      coordinator.get_current_program_model()
          .get_trace()
          .back()
          ->get_executor();
  thread_ids.erase(last_runner_to_execute);

  // O(# threads)
  {
    const model::transition &S_n =
        *coordinator.get_current_program_model().get_trace().back();

    for (runner_id_t rid : thread_ids) {
      const model::transition &next_sp =
          *coordinator.get_current_program_model()
               .get_pending_transitions()
               .get_transition_for_runner(rid);
      dynamically_update_backtrack_sets_at_index(context, S_n, next_sp,
                                                 context.stack.at(t_stack_top),
                                                 t_stack_top, rid);
    }
  }

  // O(transition stack size)
  {
    const model::transition &next_s_p_for_latest_runner =
        *coordinator.get_current_program_model()
             .get_pending_transitions()
             .get_transition_for_runner(last_runner_to_execute);

    // It only remains to add backtrack points at the necessary
    // points for thread `last_runner_to_execute`. We start at one step elow the
    // top since we know that transition to not be co-enabled (since it was, by
    // assumption, run by `last_runner_to_execute`)
    for (int i = t_stack_top - 1; i >= 0; i--) {
      const model::transition &S_i =
          *coordinator.get_current_program_model().get_trace().at(i);
      const bool should_stop = dynamically_update_backtrack_sets_at_index(
          context, S_i, next_s_p_for_latest_runner, context.stack.at(i), i,
          last_runner_to_execute);
      /*
       * Stop when we find the _first_ such i; this
       * will be the maxmimum `i` since we're searching
       * backwards
       */
      if (should_stop) break;
    }
  }
}

bool classic_dpor::dynamically_update_backtrack_sets_at_index(
    const dpor_context &context, const model::transition &S_i,
    const model::transition &next_sp, stack_item &pre_si, size_t i,
    runner_id_t p) {
  // TODO: add in co-enabled conditions
  const bool has_reversible_race = this->are_dependent(next_sp, S_i) &&
                                   this->are_coenabled(next_sp, S_i) &&
                                   !context.happens_before_thread(i, p);

  // If there exists i such that ...
  if (has_reversible_race) {
    std::set<runner_id_t> e;

    for (runner_id_t const q : pre_si.get_enabled_runners()) {
      const bool in_e = q == p || context.threads_race_after(i, q, p);

      // If E != empty set
      if (in_e && !pre_si.sleep_set_contains(q)) e.insert(q);
    }

    if (e.empty()) {
      // E is the empty set -> add every enabled thread at pre(S, i)
      for (runner_id_t const q : pre_si.get_enabled_runners())
        if (!pre_si.sleep_set_contains(q))
          pre_si.insert_into_backtrack_set_unless_completed(q);
    } else {
      for (runner_id_t const q : e) {
        // If there is a thread in preSi that we
        // are already backtracking AND which is contained
        // in the set E, chose that thread to backtrack
        // on. This is equivalent to not having to do
        // anything
        if (pre_si.backtrack_set_contains(q)) return true;
      }

      // Otherwise select an arbitrary thread to backtrack upon.
      pre_si.insert_into_backtrack_set_unless_completed(*e.begin());
    }
  }
  return has_reversible_race;
}

classic_dpor::dependency_relation_type classic_dpor::default_dependencies() {
  classic_dpor::dependency_relation_type dr;
  using namespace transitions;
  dr.register_dd_entry<const thread_create>(&thread_create::depends);
  dr.register_dd_entry<const thread_join>(&thread_join::depends);
  dr.register_dd_entry<const thread_start>(&thread_start::depends);
  dr.register_dd_entry<const thread_exit>(&thread_exit::depends);
  dr.register_dd_entry<const mutex_lock, const mutex_init>(
      &mutex_lock::depends);
  dr.register_dd_entry<const mutex_lock, const mutex_lock>(
      &mutex_lock::depends);
  dr.register_dd_entry<const condition_variable_wait,
                       const condition_variable_init>(
      &condition_variable_wait::depends);
  dr.register_dd_entry<const condition_variable_wait, const mutex_lock>(
      &condition_variable_wait::depends);
  dr.register_dd_entry<const condition_variable_signal,
                       const condition_variable_wait>(
      &condition_variable_signal::depends);
  dr.register_dd_entry<const condition_variable_signal, const mutex_lock>(
      &condition_variable_signal::depends);
  return dr;
}

classic_dpor::coenabled_relation_type classic_dpor::default_coenabledness() {
  using namespace transitions;
  classic_dpor::dependency_relation_type cr;
  cr.register_dd_entry<const thread_create>(&thread_create::coenabled_with);
  cr.register_dd_entry<const thread_join>(&thread_join::coenabled_with);
  cr.register_dd_entry<const mutex_lock, const mutex_unlock>(
      &mutex_lock::coenabled_with);
  cr.register_dd_entry<const condition_variable_signal,
                       const condition_variable_wait>(
      &condition_variable_signal::coenabled_with);
  cr.register_dd_entry<const condition_variable_signal, const mutex_unlock>(
      &condition_variable_signal::coenabled_with);
  cr.register_dd_entry<const condition_variable_broadcast,
                       const condition_variable_wait>(
      &condition_variable_broadcast::coenabled_with);
  cr.register_dd_entry<const condition_variable_broadcast, const mutex_unlock>(
      &condition_variable_broadcast::coenabled_with);
  cr.register_dd_entry<const condition_variable_destroy,
                       const condition_variable_wait>(
      &condition_variable_destroy::coenabled_with);
  cr.register_dd_entry<const condition_variable_destroy,
                       const condition_variable_signal>(
      &condition_variable_destroy::coenabled_with);
  return cr;
}

std::vector<std::vector<size_t>>
classic_dpor::dpor_context::transitive_reduction() const {
  // This code is heavily inspired by the code in the LEDA library
  // (https://leda.uni-trier.de), specifically the
  // `LEDA-7.2.2/src/graph/graph_alg/_transclosure.cpp`
  //
  // The transitive reduction R of a digraph G is the graph of the fewest arcs
  // such that a path exists between nodes u and v in G iff a path exists
  // between u and v in R. Essentially, it's the smallest graph with the same
  // dependency relation of G.
  //
  // In our case, we're considering a trace T := t1, t2, ..., tN of `N`
  // total transitions. Each transition can be thought of as a node in a digraph
  // with the edges representing "happens-before" dependencies between them. The
  // transitive reduction of this digraph then represents those transitions in
  // an "immediate" race: ti --> tj and for all transitions tk in-between,
  // either ti --/--> tk or tk --/-->tj.
  if (this->stack.empty()) return {};
  const size_t N = this->transition_stack_size();
  const size_t INF = N + 1;

  // `adj_list` holds the dependencies between every point in the trace. Nodes
  // are representing using integers and correspond to the index in the
  // transition stack.
  //
  // `e in adj_list[i] <--> happens_before(i, e)`.
  //
  // `redundant[i][j] <---> adj_list[i][j] can be removed in the transition
  // reduction`
  //
  // NOTE: For each `i`, the nodes are ordered in topological order. This is
  // because the transitions in the stack are already topologically ordered by
  // construction, and we process nodes in order.
  std::vector<std::vector<size_t>> adj_list(N);
  std::vector<std::vector<bool>> redundant(N);

  for (size_t i = 0; i < N; i++) {
    for (size_t j = i + 1; j < N; j++) {
      if (happens_before(i, j)) {
        adj_list[i].push_back(j);
        redundant[i].push_back(true);
      }
    }
  }

  // Compute Chain Decomposition
  //
  // Decomposes the DAG into a minimum number of disjoint paths (chains).
  //
  //  chain_id[v]: index of the chain containing v
  //  C[h]: list of nodes in chain h
  //
  // See Dilworth's theorem.
  std::vector<bool> unmarked(N, true);
  std::vector<size_t> chain_id(N);
  std::vector<std::list<size_t>> C;

  // Iterating in increasing topological order (see note above)
  for (size_t v = 0; v < N; ++v) {
    if (unmarked[v]) {
      C.emplace_back();  // Start a new chain
      size_t current_chain_id = C.size() - 1;
      size_t current_node = v;

      while (current_node != INF) {
        C.back().push_back(current_node);
        chain_id[current_node] = current_chain_id;
        unmarked[current_node] = false;

        // Find the first unmarked neighbor 'w' (smallest topological rank)
        size_t next_node = INF;
        for (size_t w : adj_list[current_node]) {
          if (unmarked[w]) {
            next_node = w;
            break;
          }
        }
        current_node = next_node;
      }
    }
  }

  // Compute Reachability and Redundancy
  //
  // reach[i][c] = min{j ; node j in C[c] and path i -> j exists}
  //
  // where "<" refers to topological order
  //
  // Intuitively, `reach[i][c]` denotes the "smallest" (topologically-speaking)
  // node in chain `c` to which `i` has a path.
  const int num_chains = C.size();
  std::vector<std::vector<int>> reach(N, std::vector<int>(num_chains, INF));

  // The key loop: iterates in DECREASING topological order
  for (size_t k = 0; k <= N - 1; k++) {
    size_t v = N - k - 1;

    // Process outgoing edges of v (target 'w' is in INCREASING order)
    for (size_t i = 0; i < adj_list[v].size(); i++) {
      const int w = adj_list[v][i];
      if (w < reach[v][chain_id[w]]) {
        redundant[v][i /* INDEX of w */] = false;

        // If v can reach w via a non-redundant edge, v can reach everything j
        // reaches.
        for (int c = 0; c < num_chains; ++c)
          reach[v][c] = std::min(reach[v][c], reach[w][c]);
      }
    }
    reach[v][chain_id[v]] = v;
  }

  // Remove redundant edges from the adjacency list
  for (size_t k = 0; k < N; k++) {
    auto &out_edges = adj_list[k];
    auto &redundant_edges = redundant[k];
    assert(out_edges.size() == redundant_edges.size());
    size_t j = 0;
    for (size_t i = 0; i < out_edges.size(); ++i)
      if (!redundant_edges[i]) out_edges[j++] = out_edges[i];
    out_edges.resize(j);
  }

#if DEBUG
  // Validation: every point in the adjacency list happens before
  for (size_t i = 0; i < N; i++)
    for (size_t j = i + 1; j < N; j++)
      if (std::find(adj_list[i].begin(), adj_list[i].end(), j) !=
          adj_list[i].end()) {
        assert(happens_before(i, j));
      } else {
        if (happens_before(i, j)) {
          auto s = std::find_if(adj_list[i].begin(), adj_list[i].end(),
                                [&](int w) { return happens_before(w, j); });
          assert(s != adj_list[i].end());
        }
        // Not in adj so ok in `false` case
      }
#endif
  return adj_list;
}

#ifdef MCMINI_USE_SCIP
void SCIP_dump(SCIP *scip) {
  SCIP_CALL_ABORT(SCIPprintOrigProblem(scip, nullptr, "lp", false));
}
#endif

std::vector<const transition *> classic_dpor::dpor_context::linearize_optimal()
    const {
#ifdef MCMINI_USE_SCIP
  // Given a fixed trace `T` of `N` transitions and a happens-before relation
  // `happens-before_T: [1, N] x [1, N] -> {0, 1}` over the indices of `T`, the
  // goal is to produce a new trace `T'` such that `happens-before_T(i, j) =
  // happens-before_T'(i, j)` and such that the number of inversions is
  // minimized. An inversion is a point in the trace where the thread ID of the
  // `i`th transition doesn't match the thread ID of the `(i + 1)`th transition.
  //
  // The intuition in producing such a trace `T'` is that reasoning about this
  // trace is "easier" than any other because the number of context switches
  // (inversions) a user must perform to analyze a new trace is minimized.
  // Moreover, since `T'` obeys the same "happens-before" relation as the
  // original trace `T`, it produces the exact same bug.
  //
  // We can map relinearization into a graph problem. Let `Tids := {tid : exists
  // i, T[i]->get_executor() = tid }`. We construct an unweighted, labeled
  // digraph `K` where
  //
  //    1. V(K) := {1, 2, ..., N}
  //    2. E(K) := {(i, j) in [1, N] x [1, N] : happens-before_T(i, j)}
  //    3. tid : V -> Tids, tid(v) := T[v].get_executor()
  //
  // Since `happens-before_T` is transitive, we note that `K =
  // Transitive-Closure(K)` since `happens-before_T` is transitive. Moreover,
  // the array `[1, 2, ..., N]` is a valid topological sorting of `K` since `i >
  // j --> happens-before_T(i, j) = 0`.
  //
  // An equivalent formulation of the problem is to find a topological sorting
  // of K of minimal inversions. Computing such a minimal topological sort can
  // be reduced to the sequential ordering problem (SOP) (see
  // https://people.idsia.ch/~roberto/SOP.pdf for a reference). The SOP problem
  // is related to the Asymmetric Traveling Salesperson Problem (ATSP). In the
  // ATSP, the goal is to find a Hamiltonian Cycle in a weighted directed graph
  // of minimal weight. SOP adds one more constraint and requires that certain
  // nodes are visited before others based on a relation `R` over the vertices.
  //
  // We construct a new graph `G` from `K`. Here, we
  // note that although the SOP problem and ATSP problems work with cycles, we
  // can add a source and sink node to our DAG with zero weight edges between
  // each other and the respective start and end nodes of the digraph (those
  // with in-degree and out-degree equalt to 0 respectively) to map exactly to
  // the SOP problem.
  //
  // Denote `H := Transitive-Reduction(K)`. Recall each node `v` in `K` and by
  // proxy `H` corresponds to a transition in the transition stack `T`. Let
  // `tid(v)` denote the labeling function of `K`. Then let
  //
  //    1. V(G) := V(H) + {source, sink}
  //    2. E(G) := V(H) x V(H) + {(u, sink) : u in V(H), out-degree[u] = 0} +
  //    {(source, u) : u in V(H), in-degree[u] = 0}}
  //    3. For all e = (u, v) in E(G),
  //
  //       w(e) := if tid(u) == tid(v) || (u == source) || (v == sink) then 0
  //       else 1
  //
  // Let `C := SOP(G, E(H))` denote the Hamiltonian Cycle of minimum weight
  // subjet to `E(H)`. Then `C := m_1, m_2, ..., m_N` corresponds to the minimal
  // topological sorting. Clearly, `C` is a valid topological sorting since it
  // contains all nodes of `H` and in an order obeying `E(H)`. By construction,
  // the total weight of `C` equals the number of inversions in `C`, since an
  // edge is weighted iff they are inverted and don't connect either the sink or
  // source. Since `C` is of minimal weight,. it is also of minimal inversions.
  //
  // The SOP instance can be solved by reduction using integer constraints.
  // Here, we use the Miller-Tucker-Zemlin (MTZ) formulation of the TSP problem.
  // In MTZ, a variable `x_ij in {0, 1}` is added to represent whether the edge
  // `(i, j)` is taken. For eache node `v_i`, a variable `u_i` is added to
  // indicate the order the node is visited in the cycle. Constraints are added
  // to ensure that exactly one `x_ij` goes into and exits each node and such
  // that if (i, j) is taken then `u_i < u_j` We can adjust the MTZ to work on
  // an SOP instance by adding precendence constraints on the variables `u_i` by
  // always forcing `u_i <= u_j + 1` iff `i` happens-before `j`.
  //
  // Reducing the number of variables is key to making the solver efficient.
  // There are two optimizations we apply in the context of our specific
  // optimization problem instance:
  //
  // A) For each edge (u, v) in E(H), only (u, v) needs to be added to G
  // since a path that includes (v, u) would not satisfy the ordering
  // constraint. Moreover, any edge (u', v') in E(K) such that there exists
  // a node k such that (u', k) and (k, v') in E(H) can be eliminated from
  // G. This is equivalent to eliminating any edges in G that are contained
  // in the `Transitive-Closure(H) = K`. In general,
  //
  //    1. V(G) := V(H) + {source, sink}
  //    2. E(G) := E(H) + {(u, v) in V(H) x V(H) : (u, v) and (v, u) not in K} +
  //    {(u, sink) : u in V(H), out-degree[u] = 0} +
  //    {(source, u) : u in V(H), in-degree[u] = 0}}
  //    3. For all e = (u, v) in E(G),
  //
  //       w(e) := if tid(u) == tid(v) || (u == source) || (v == sink) then 0
  //       else 1
  //
  // B) We observe the following. Firstly, for any two nodes `(u, v) in
  // H_TC`, `tid[u] = tid[v] ==> u --> v`. I.e., there is always a
  // happens-before relation between two nodes of the same label.
  //
  // For each sequence V = v_1, v_2, ..., v_r in E(H) such that
  //
  //    1. For all i in [1, r) out-degree[u_i] = in-degree[u_(i + 1)] = 1
  //
  //    V must appear in the SOP Hamiltonian cycle C of minimal
  //    weight.
  //
  // Informally, given a contiguous "block" of transitions which must appear in
  // order but are otherwise independent with other transitions except possibly
  // the first and last transitions of the block, anything that doesn't
  // "happen-before" the first transition in the block can be placed anywhere
  // inside the block (otherwise there would be a transitive dependency
  // somewhere in the block). Since the block ordering "forces" a certain
  // ordering of the transitions, the number of inversions `I` in the block is
  // fixed. For any `j` such that `v_1 --/--> j`, `tid[v_1] != tid[j]` and
  // by transitivity `u --/--> j` and hence `tid[u] != tid[j]` for all u in V.
  // `u --/--> j` further implies `(u, j) and (j, u) in G` for all `u in C`.
  // Since `tid[u] != tid[j]`, `w_uj = w_ju = 1`. Since any Hamiltonian cycle
  // must include all nodes in order, either the edge `(v_1, v_2)` is included
  // or `(v_1, u_1), ..., (u_n, v_2)` for some `u_l` sequence such that `v_1
  // --\--> u_1`. Since w_(v_1, v_2) <= 1 but `w_(v_1, u_1) = w_(u_n, v_2)`, the
  // weight of taking the interior edge instead of two exterior ones smaller
  // than taking the two outer ones. We can essentially treat (v_1, v_2), ...,
  // (v_(r-1), v_r) as a subgraph with all incoming and outgoing edges of
  // weight 1.
  //
  // Consequently, we can first group together simple chains and solve the
  // smaller SOP instance and still obtain the same optimal result.
  //
  // To compute the reduced graph, we use the Disjoint Set Union. See
  // https://cp-algorithms.com/data_structures/disjoint_set_union.html
  struct dsu {
    std::vector<size_t> parent;
    std::vector<size_t> sizes;

   public:
    dsu(size_t n) : sizes(n, 1) {
      parent.resize(n);
      std::iota(parent.begin(), parent.end(), 0);
    }

    size_t find(size_t i) {
      if (parent[i] == i) return i;
      return parent[i] = find(parent[i]);
    }

    void unite(size_t i, size_t j) {
      size_t root_i = find(i);
      size_t root_j = find(j);
      if (root_i != root_j) {
        if (sizes[root_i] < sizes[root_j]) std::swap(root_i, root_j);
        parent[root_i] = root_j;
        sizes[root_i] += sizes[root_j];
      }
    }
  };

  // Let `H_red` be the edge-reduced, transitive reduction graph. For each edge
  // `(u, v) in E(H)`, if out(u) = in(v) = 1, then we can effectively combine
  // `u` and `v`  together into a single node iff they refer to transitions run
  // by the same thread. `supernodes` maps nodes in the reduced graph to the
  // original graph `H`.
  //
  // ```
  // Where `i` refers to a node ID in the reduced graph H_red
  // supernodes[i] = [u_1, u_2, ..., u_(l+1)] <---> (u_j, u_{j+1}) in V(H) and
  // out-degree[u_j] = in-degree[u_(j+1)] = 1
  // ```
  // Where `i` refers to a member of V(H)
  // dsu.find(i) = j <---> i is a part of supernode j in the reduced graph
  //```
  // The immediate transitive dependencies of `supernodes[i].back()` = u_(l+1)
  // are the same as the transitive dependencies of the supernode. Moreoever,
  // using the argument in the proof above, the set of edges
  // {(u_i, v) in V(H) x V(H) : (u_i, v) and (v, u_i) not in K} is the same for
  // any u_i. Here, we pick the first one `supernodes[i].front()`
  const auto H = this->transitive_reduction();
  const size_t N = H.size();
  std::vector<size_t> out_degree(N, 0);
  std::vector<size_t> in_degree(N, 0);

  // INVARIANT: supernodes[i] is sorted by ID (and hence sorted in topological
  // order)
  std::vector<std::vector<size_t>> supernodes;
  std::unordered_map<size_t, size_t> H_to_supernode;
  {
    dsu dsu(N);
    {
      std::vector<size_t> tids(N, 0);

      for (size_t u = 0; u < N; ++u) {
        out_degree[u] = H[u].size();
        tids[u] = tid(u);
        for (size_t v : H[u]) in_degree[v]++;
      }

      for (size_t u = 0; u < N; ++u) {
        if (out_degree[u] == 1) {
          size_t v = H[u][0];  // The only neighbor
          if (in_degree[v] == 1 && tids[u] == tids[v]) dsu.unite(u, v);
        }
      }
    }
    {
      std::map<size_t, std::vector<size_t>> groups;
      for (size_t i = 0; i < N; ++i) groups[dsu.find(i)].push_back(i);

      supernodes.reserve(groups.size());
      for (auto &p : groups) {
        const size_t new_supernode_id = supernodes.size();
        for (size_t i : p.second) H_to_supernode[i] = new_supernode_id;
        supernodes.push_back(std::move(p.second));
      }
    }
  }

  // In what follows, there are implicitly two graphs:
  //
  // 1. The original transitive reduction `H` containing the "happens-before"
  // DAG wihtout transitive dependencies with `N` nodes.
  //
  // 2. The reduced DAG `H_red` with `M <= N` nodes where nodes `(u, v)` in `H`
  // where `out-degree[u] = in-degree[v] = 1` are combined together to reduce
  // the number of variables. This graph isn't explicitly stored in memory.
  // Instead, for each node in `H_red`, a list of back references in `H`
  // indicating
  //
  // Iterations happen over nodes IDs in `H_red`. There a few important
  // observations (here `i`s and `j`s are over super node indices)
  // ```
  // supernode[i].front() --/--> supernode[j].front() --> i -->_S' j
  // out_degree_super[i] = out_degree[supernodes[i].back()]
  // in_degree_super[i] = in_degree[supernodes[i].front()]
  SCIP *scip = nullptr;
  SCIPcreate(&scip);
  SCIPsetMessagehdlr(scip, NULL);
  SCIPincludeDefaultPlugins(scip);
  SCIPcreateProbBasic(scip, "Minimize_Inversions");
  SCIPsetObjsense(scip, SCIP_OBJSENSE_MINIMIZE);

  const size_t M = supernodes.size();
  std::vector<SCIP_VAR *> u(M);
  std::vector<std::vector<SCIP_VAR *>> x(
      M + 1, std::vector<SCIP_VAR *>(M + 1, nullptr));

  // Path variables: 1 <= u_i <= M
  for (size_t i = 0; i < M; i++) {
    const std::string name = "u_" + std::to_string(i);
    SCIPcreateVarBasic(scip, &u[i], name.c_str(), 1.0, (double)M, 0.0,
                       SCIP_VARTYPE_CONTINUOUS);
    SCIPaddVar(scip, u[i]);
  }

  // All edges in the transitive reduction, add forward edge x_ij in {0, 1}
  for (size_t i = 0; i < M; ++i) {
    for (size_t j : H[supernodes[i].back()]) {
      assert(supernodes[H_to_supernode[j]].front() == j);
      j = H_to_supernode[j];
      assert(i < j);
      assert(x[i][j] == nullptr);
      std::string name = "x_" + std::to_string(i) + std::to_string(j);
      SCIPcreateVarBasic(
          scip, &x[i][j], name.c_str(), 0.0, 1.0,
          tid(supernodes[i].back()) != tid(supernodes[j].front()),
          SCIP_VARTYPE_BINARY);
      SCIPaddVar(scip, x[i][j]);
    }
  }

  // All edges not contained in the transitive closure
  // Add forward _and_ backward edges x_ij and x_ji
  for (size_t i = 0; i < M; ++i)
    for (size_t j = i + 1; j < M; j++)
      if (!happens_before(supernodes[i].front(), supernodes[j].front())) {
        assert(x[i][j] == nullptr);
        assert(x[j][i] == nullptr);
        const double w_e =
            tid(supernodes[i].back()) != tid(supernodes[j].front());
        {
          const std::string name = "x_" + std::to_string(i) + std::to_string(j);
          SCIPcreateVarBasic(scip, &x[i][j], name.c_str(), 0.0, 1.0, w_e,
                             SCIP_VARTYPE_BINARY);
          SCIPaddVar(scip, x[i][j]);
        }
        {
          const std::string name = "x_" + std::to_string(j) + std::to_string(i);
          SCIPcreateVarBasic(scip, &x[j][i], name.c_str(), 0.0, 1.0, w_e,
                             SCIP_VARTYPE_BINARY);
          SCIPaddVar(scip, x[j][i]);
        }
      }

  // Source/sink variables --> only added to DAG's entrances + exits
  for (size_t i = 0; i < M; ++i) {
    if (out_degree[supernodes[i].back()] == 0) {
      const std::string name = "x_" + std::to_string(i) + std::to_string(M);
      SCIPcreateVarBasic(scip, &x[i][M], name.c_str(), 0.0, 1.0, 0.0,
                         SCIP_VARTYPE_BINARY);
      SCIPaddVar(scip, x[i][M]);
    }

    if (in_degree[supernodes[i].front()] == 0) {
      const std::string name = "x_" + std::to_string(M) + std::to_string(i);
      SCIPcreateVarBasic(scip, &x[M][i], name.c_str(), 0.0, 1.0, 0.0,
                         SCIP_VARTYPE_BINARY);
      SCIPaddVar(scip, x[M][i]);
    }
  }

  // MTZ Constraints
  // A. Exactly one exit and one entrance per node (including source/sink)
  for (size_t i = 0; i < M + 1; ++i) {
    SCIP_CONS *enter_cons = nullptr;
    SCIP_CONS *leave_cons = nullptr;
    const std::string enter = "enter_" + std::to_string(i);
    const std::string leave = "leave_" + std::to_string(i);
    SCIPcreateConsBasicLinear(scip, &enter_cons, enter.c_str(), 0, nullptr,
                              nullptr, 1.0, 1.0);
    SCIPcreateConsBasicLinear(scip, &leave_cons, leave.c_str(), 0, nullptr,
                              nullptr, 1.0, 1.0);
    for (size_t j = 0; j < M + 1; ++j) {
      if (x[i][j]) SCIPaddCoefLinear(scip, leave_cons, x[i][j], 1.0);
      if (x[j][i]) SCIPaddCoefLinear(scip, enter_cons, x[j][i], 1.0);
    }
    SCIPaddCons(scip, enter_cons);
    SCIPaddCons(scip, leave_cons);
    SCIPreleaseCons(scip, &enter_cons);
    SCIPreleaseCons(scip, &leave_cons);
  }

  // B. MTZ Subtour Elimination:
  // -INF <= u_i - u_j + M*x_ij <= (M - 1) for i, j in [1, M], i != j
  for (size_t i = 0; i < M; ++i) {
    for (size_t j = 0; j < M; ++j) {
      if (!x[i][j]) continue;
      SCIP_CONS *cons = nullptr;
      const std::string name =
          "subtour_" + std::to_string(i) + "_" + std::to_string(j);
      SCIPcreateConsBasicLinear(scip, &cons, name.c_str(), 0, nullptr, nullptr,
                                -SCIPinfinity(scip), (double)(M - 1));
      SCIPaddCoefLinear(scip, cons, u[i], 1.0);
      SCIPaddCoefLinear(scip, cons, u[j], -1.0);
      SCIPaddCoefLinear(scip, cons, x[i][j], (double)M);
      SCIPaddCons(scip, cons);
      SCIPreleaseCons(scip, &cons);
    }
  }

  // C. Precedence Constraints: INF >= u_target - u_i >= 1
  for (size_t i = 0; i < M; ++i) {
    for (size_t target : H[supernodes[i].back()]) {
      target = H_to_supernode[target];
      SCIP_CONS *cons = nullptr;
      const std::string name =
          "precedence_" + std::to_string(i) + "_" + std::to_string(target);
      SCIPcreateConsBasicLinear(scip, &cons, name.c_str(), 0, nullptr, nullptr,
                                1.0, SCIPinfinity(scip));
      SCIPaddCoefLinear(scip, cons, u[target], 1.0);
      SCIPaddCoefLinear(scip, cons, u[i], -1.0);
      SCIPaddCons(scip, cons);
      SCIPreleaseCons(scip, &cons);
    }
  }
  SCIPsolve(scip);
  SCIP_SOL *sol = SCIPgetBestSol(scip);

  std::vector<size_t> linearization;
  struct NodePosition {
    size_t id;
    double u_value;
  };

  if (sol != nullptr) {
    std::vector<NodePosition> sequence;
    sequence.reserve(M);
    for (size_t i = 0; i < M; ++i) {
      double val = SCIPgetSolVal(scip, sol, u[i]);
      sequence.push_back({i, val});
    }
    std::sort(sequence.begin(), sequence.end(),
              [](const NodePosition &a, const NodePosition &b) {
                return a.u_value < b.u_value;
              });
    for (const auto &item : sequence)
      std::move(supernodes[item.id].begin(), supernodes[item.id].end(),
                std::back_inserter(linearization));
  } else {
    std::cerr << "No solution found!" << std::endl;
    return {};
  }
  {
    for (size_t i = 0; i < M + 1; ++i) {
      if (i < M) SCIPreleaseVar(scip, &u[i]);
      for (size_t j = 0; j < M + 1; ++j) {
        if (x[i][j]) SCIPreleaseVar(scip, &x[i][j]);
      }
    }
    SCIPfree(&scip);
  }

  std::vector<const transition *> linearized_trace(N, nullptr);
  for (size_t i = 0; i < N; i++)
    linearized_trace[i] = get_transition(linearization[i]);
  return linearized_trace;
#else
  // Fallback to greedy if SCIP isn't available
  return linearize_lowest_first();
#endif
}

std::vector<const transition *>
classic_dpor::dpor_context::linearize_lowest_first() const {
  const auto adj_list = this->transitive_reduction();
  const size_t N = adj_list.size();
  const size_t INF = N + 1;
  std::vector<size_t> linearization;
  std::vector<size_t> ready_set;
  std::vector<int> in_degree(N, 0);
  for (size_t u = 0; u < N; ++u)
    for (size_t v : adj_list[u]) in_degree[v]++;

  for (size_t i = 0; i < N; ++i)
    if (in_degree[i] == 0) ready_set.push_back(i);

  runner_id_t last_rid = RUNNER_ID_MAX;
  while (!ready_set.empty()) {
    size_t next_vertex = INF;
    size_t selected_index = INF;
    if (last_rid != RUNNER_ID_MAX) {
      for (size_t i = 0; i < ready_set.size(); ++i) {
        const runner_id_t r = get_transition(ready_set[i])->get_executor();
        if (r == last_rid) {
          next_vertex = ready_set[i];
          selected_index = i;
          break;  // Found the best greedy match that avoids inversion
        }
      }
    }

    // If no ID match, or at the start, pick the thread with the lowest id
    if (next_vertex == INF) {
      runner_id_t min_rid = RID_INVALID;
      for (size_t i = 0; i < ready_set.size(); ++i) {
        const runner_id_t r = get_transition(ready_set[i])->get_executor();
        if (r < min_rid) {
          next_vertex = ready_set[i];
          selected_index = i;
          min_rid = r;
        }
      }
    }

    linearization.push_back(next_vertex);
    last_rid = get_transition(next_vertex)->get_executor();

    // Remove the selected vertex from the ready set
    // Swap with the back and pop for O(1) removal from vector (since order in S
    // doesn't matter)
    std::swap(ready_set[selected_index], ready_set.back());
    ready_set.pop_back();

    for (int v : adj_list[next_vertex]) {
      in_degree[v]--;
      if (in_degree[v] == 0) {
        ready_set.push_back(v);
      }
    }
  }

  std::vector<const transition *> linearized_trace(N, nullptr);
  for (size_t i = 0; i < N; i++)
    linearized_trace[i] = get_transition(linearization[i]);
  return linearized_trace;
}
