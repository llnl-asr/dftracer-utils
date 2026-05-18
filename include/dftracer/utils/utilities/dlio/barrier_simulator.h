#ifndef DFTRACER_UTILS_UTILITIES_DLIO_BARRIER_SIMULATOR_H
#define DFTRACER_UTILS_UTILITIES_DLIO_BARRIER_SIMULATOR_H

#include <dftracer/utils/utilities/dlio/statistic.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <vector>

namespace dftracer::utils::utilities::dlio {

using Rng = std::mt19937_64;
using Sampler = std::function<double(Rng&)>;

struct BarrierSimulatorContext {
    int num_ranks = 0;
    int num_steps = 0;
    double trace_e2e_duration = 0.0;

    std::vector<std::vector<double>> fetch_block_trace;
    std::vector<std::vector<double>> fetch_iter_trace;

    Statistic fetch_block_stats;
    Statistic fetch_iter_stats;
    Statistic preprocess_stats;
    Statistic getitem_stats;

    ComponentTimeMetrics trace_preprocess_metrics;
    ComponentTimeMetrics trace_fetch_iter_metrics;
    ComponentTimeMetrics trace_fetch_block_metrics;
    double trace_rank_variance = 0.0;
    std::vector<double> trace_per_rank_throughput;

    std::optional<std::vector<std::vector<double>>> getitem_trace;
    std::optional<Statistic> io_stats;
    std::optional<std::vector<double>> io_samples;

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

    std::vector<double> per_rank_completion_time;
    double rank_variance = 0.0;
    double trace_rank_variance = 0.0;
    double rank_variance_error = 0.0;
    double load_imbalance = 0.0;

    std::vector<double> simulated_fetch_block;
    double fetch_block_cdf_similarity = 0.0;

    std::vector<double> simulated_preprocess;
    std::vector<double> simulated_getitem;
    std::vector<double> simulated_fetch_iter;
    double fetch_iter_cdf_similarity = 0.0;
    double getitem_cdf_similarity = 0.0;
    double avg_queue_depth = 0.0;
    double avg_queue_stalls = 0.0;

    ComponentTimeMetrics preprocess_metrics;
    ComponentTimeMetrics fetch_iter_metrics;
    ComponentTimeMetrics fetch_block_metrics;

    std::optional<ComponentTimeMetrics> trace_preprocess_metrics;
    std::optional<ComponentTimeMetrics> trace_fetch_iter_metrics;
    std::optional<ComponentTimeMetrics> trace_fetch_block_metrics;

    std::vector<double> simulated_per_rank_throughput;
    std::vector<double> trace_per_rank_throughput;

    double throughput_mean = 0.0;
    double trace_throughput_mean = 0.0;
    double throughput_mean_error = 0.0;

    double throughput_variance = 0.0;
    double trace_throughput_variance = 0.0;

    double throughput_cdf_similarity = 0.0;
};

class BarrierSimulator {
   public:
    // preprocess_sampler may be empty; pass {} to use trace-derived
    // getitem/preprocess stats.
    BarrierSimulationResult simulate(
        const BarrierSimulatorContext& context, std::uint64_t base_seed,
        const Sampler& fetch_block_sampler,
        const Sampler& preprocess_sampler = {}) const;
};

// 1 - Kolmogorov-Smirnov statistic between the two empirical distributions.
// Returns 1.0 for perfect match, 0.0 for fully disjoint.
double cdf_similarity(const std::vector<double>& a,
                      const std::vector<double>& b);

double variance(const std::vector<double>& values);

}  // namespace dftracer::utils::utilities::dlio

#endif
