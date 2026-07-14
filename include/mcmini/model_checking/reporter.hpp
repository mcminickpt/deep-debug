#pragma once
#include "mcmini/model/config.hpp"
#include "mcmini/model/exception.hpp"
#include "mcmini/model_checking/algorithm.hpp"

namespace model_checking {
class reporter : public algorithm::callbacks {
private:
  const bool verbose;
  const bool relinearize_traces;
  const bool use_optimal_linearization;

private:
  void dump_relinearized_trace(std::ostream &, const algorithm::context &,
                               const stats &) const;

public:
  reporter() = default;
  reporter(const model::config &c)
      : verbose(c.verbose),
        relinearize_traces(c.relinearize_traces || c.use_optimal_linearization),
        use_optimal_linearization(c.use_optimal_linearization) {}

public:
  // void crash(const algorithm::context &, const stats &) const override;
  void deadlock(const algorithm::context &, const stats &) const override;
  // void data_race(const algorithm::context &, const stats &) const override;
  void trace_completed(const algorithm::context &,
                       const stats &) const override;
  void abnormal_termination(
      const algorithm::context &, const stats &,
      const real_world::process::termination_error &) const override;
  // void nonzero_exit_code(
  //     const algorithm::context &, const stats &,
  //     const real_world::process::nonzero_exit_code_error &) const override;
  void undefined_behavior(
      const algorithm::context &, const stats &,
      const model::undefined_behavior_exception &) const override;
};
} // namespace model_checking