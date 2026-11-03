// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_DLIO_BARRIER_SIMULATOR_H
#define DFTRACER_UTILS_UTILITIES_DLIO_BARRIER_SIMULATOR_H

#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/dlio/statistic.h>

#include <cstdint>
#include <random>

namespace dftracer::utils::utilities::dlio {

using Rng = std::mt19937_64;
using Sampler = dstd::Function<double(Rng&)>;

struct BarrierSimulatorContext {
    int num_ranks = 0;
    int num_steps = 0;
    double trace_e2e_duration = 0.0;

    dstd::Vector<dstd::Vector<double>> fetch_block_trace;
    dstd::Vector<dstd::Vector<double>> fetch_iter_trace;

    Statistic fetch_block_stats;
    Statistic fetch_iter_stats;
    Statistic preprocess_stats;
    Statistic getitem_stats;

    ComponentTimeMetrics trace_preprocess_metrics;
    ComponentTimeMetrics trace_fetch_iter_metrics;
    ComponentTimeMetrics trace_fetch_block_metrics;
    double trace_rank_variance = 0.0;
    dstd::Vector<double> trace_per_rank_throughput;

    dstd::Optional<dstd::Vector<dstd::Vector<double>>> getitem_trace;
    dstd::Optional<Statistic> io_stats;
    dstd::Optional<dstd::Vector<double>> io_samples;

    bool sync_mode = false;
    int accumulate_grad_batches = 1;
    bool enable_preprocess_simulation = false;
    int num_workers = 8;
    int prefetch_factor = 2;
    double preprocess_slowdown_factor = 1.0;
    double base_fetch_iter_overhead = 0.0;
    bool is_aggregated_trace = false;
    double avg_calls_per_epoch = 1.0;
};

struct BarrierSimulationResult {
    double e2e_duration = 0.0;
    double e2e_error = 0.0;

    double avg_barrier_overhead = 0.0;
    double max_barrier_overhead = 0.0;

    dstd::Vector<double> per_rank_completion_time;
    double rank_variance = 0.0;
    double trace_rank_variance = 0.0;
    double rank_variance_error = 0.0;
    double load_imbalance = 0.0;

    dstd::Vector<double> simulated_fetch_block;
    double fetch_block_cdf_similarity = 0.0;

    dstd::Vector<double> simulated_preprocess;
    dstd::Vector<double> simulated_getitem;
    dstd::Vector<double> simulated_fetch_iter;
    double fetch_iter_cdf_similarity = 0.0;
    double getitem_cdf_similarity = 0.0;
    double avg_queue_depth = 0.0;
    double avg_queue_stalls = 0.0;

    ComponentTimeMetrics preprocess_metrics;
    ComponentTimeMetrics fetch_iter_metrics;
    ComponentTimeMetrics fetch_block_metrics;

    dstd::Optional<ComponentTimeMetrics> trace_preprocess_metrics;
    dstd::Optional<ComponentTimeMetrics> trace_fetch_iter_metrics;
    dstd::Optional<ComponentTimeMetrics> trace_fetch_block_metrics;

    dstd::Vector<double> simulated_per_rank_throughput;
    dstd::Vector<double> trace_per_rank_throughput;

    double throughput_mean = 0.0;
    double trace_throughput_mean = 0.0;
    double throughput_mean_error = 0.0;

    double throughput_variance = 0.0;
    double trace_throughput_variance = 0.0;

    double throughput_cdf_similarity = 0.0;
};

class BarrierSimulator {
   public:
    /// preprocess_sampler may be empty; pass {} to use trace-derived
    /// getitem/preprocess stats.
    BarrierSimulationResult simulate(
        const BarrierSimulatorContext& context, std::uint64_t base_seed,
        const Sampler& fetch_block_sampler,
        const Sampler& preprocess_sampler = {}) const;
};

/// 1 - Kolmogorov-Smirnov statistic between the two empirical distributions.
/// Returns 1.0 for perfect match, 0.0 for fully disjoint.
double cdf_similarity(const dstd::Vector<double>& a,
                      const dstd::Vector<double>& b);

double variance(const dstd::Vector<double>& values);

}  // namespace dftracer::utils::utilities::dlio

#endif
