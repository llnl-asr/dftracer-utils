#ifndef DFTRACER_UTILS_UTILITIES_DLIO_OPTIMIZER_H
#define DFTRACER_UTILS_UTILITIES_DLIO_OPTIMIZER_H

#include <dftracer/utils/utilities/common/statistics/mixture.h>
#include <dftracer/utils/utilities/dlio/barrier_simulator.h>

#include <cstdint>
#include <vector>

namespace dftracer::utils::utilities::dlio {

using BestModel = ::dftracer::utils::utilities::common::statistics::BestModel;

struct OptimizerOptions {
    int max_iterations = 5;
    double target_e2e_error = 0.05;
    double target_cdf_similarity = 0.90;
    int patience = 10;
    double epsilon = 1.0;
    double momentum = 0.9;
    double min_percentile = 50.0;
    double initial_percentile = 95.0;
    std::uint64_t base_seed = 42;
};

struct OptimizerResult {
    BarrierSimulationResult best;
    double best_percentile = 0.0;
    int iterations_used = 0;
    bool converged = false;
};

// Sequential momentum-based optimizer.
// Searches for the max_bound percentile that minimizes simulator e2e_error
// while preserving fetch_block_cdf_similarity. `sample_times` is the sorted
// flat per-call sample array used for percentile lookups (sorted in-place if
// not).
//
// Each iteration:
//   1. comp_max_bound = percentile(sample_times, current_percentile)
//   2. comp_sampler = make_sampler(model, min=sample_times.front(),
//   max=comp_max_bound)
//   3. result = simulator.simulate(context, base_seed, comp_sampler)
//   4. Adjust current_percentile via momentum-smoothed step proportional to
//   error.
OptimizerResult optimize_max_bound_percentile(
    const BarrierSimulatorContext& context, const BestModel& model,
    std::vector<double> sample_times, const OptimizerOptions& options = {});

// Helper: percentile by sorted index (linear interpolation between adjacent
// samples). Returns 0 if data is empty.
double percentile(const std::vector<double>& sorted_data, double pct);

}  // namespace dftracer::utils::utilities::dlio

#endif
