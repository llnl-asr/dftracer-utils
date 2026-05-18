#ifndef DFTRACER_UTILS_UTILITIES_DLIO_TRACE_LOADER_H
#define DFTRACER_UTILS_UTILITIES_DLIO_TRACE_LOADER_H

#include <dftracer/utils/utilities/common/statistics/ddsketch.h>
#include <dftracer/utils/utilities/dlio/barrier_simulator.h>
#include <dftracer/utils/utilities/dlio/statistic.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::dlio {

using DDSketch = ::dftracer::utils::utilities::common::statistics::DDSketch;

// DLIO category / event names matched in the AGGREGATION CF.
inline constexpr std::string_view CATEGORY_DATALOADER = "dataloader";
inline constexpr std::string_view CATEGORY_DATA = "data";
inline constexpr std::string_view EVENT_FETCH_BLOCK = "fetch.block";
inline constexpr std::string_view EVENT_FETCH_ITER = "fetch.iter";
inline constexpr std::string_view EVENT_PREPROCESS = "preprocess";
inline constexpr std::string_view EVENT_ITEM = "item";

struct AggregatedTraces {
    // Per-rank concatenated sample sequences (seconds), in pid-ascending,
    // then time-bucket-ascending order.
    std::vector<std::vector<double>> fetch_block_trace;
    std::vector<std::vector<double>> fetch_iter_trace;
    std::vector<std::vector<double>> getitem_trace;

    // Flat sample arrays for distribution fitting (seconds).
    std::vector<double> computation_times;  // fetch.block
    std::vector<double> preprocess_times;   // preprocess

    // Sketches merged across all (rank, bucket) entries for each component.
    // Nullable: only populated if the aggregator was run with
    // --compute-percentiles.
    std::shared_ptr<DDSketch> fetch_block_sketch;
    std::shared_ptr<DDSketch> fetch_iter_sketch;
    std::shared_ptr<DDSketch> preprocess_sketch;
    std::shared_ptr<DDSketch> getitem_sketch;

    // Statistics with min/max/mean/count populated, sketch attached when
    // present.
    Statistic fetch_block_stats;
    Statistic fetch_iter_stats;
    Statistic preprocess_stats;
    Statistic getitem_stats;

    // Aggregate metrics (seconds).
    double trace_e2e_duration = 0.0;
    double trace_rank_variance = 0.0;
    std::vector<double> trace_per_rank_throughput;

    // Per-component accumulated / union times. Both computed from actual
    // (ts, te) boundaries in the CF rather than a fixed-ratio heuristic.
    ComponentTimeMetrics trace_preprocess_metrics;
    ComponentTimeMetrics trace_fetch_iter_metrics;
    ComponentTimeMetrics trace_fetch_block_metrics;

    // Discovered PIDs (sorted) - index in this list defines rank ID.
    std::vector<std::uint64_t> rank_pids;

    int num_ranks = 0;
    int num_steps = 0;  // min length across ranks of fetch_block_trace

    // True if any AGGREGATION entry was found.
    bool any_data = false;

    // True if at least one DDSketch was present in the CF. Drives whether
    // sample synthesis uses inverse-CDF or mean replication.
    bool sketches_available = false;

    // Time bucket width in microseconds (from AggGlobalConfig).
    std::uint64_t time_interval_us = 0;
};

struct TraceLoaderOptions {
    // Hard cap on samples synthesized per (cat, name, pid, bucket) entry, so a
    // single high-count bucket cannot blow up memory. 0 disables the cap.
    std::uint64_t max_samples_per_entry = 100;
    // Seed for inverse-CDF sketch sampling.
    std::uint64_t seed = 0xD15710;
};

// Loads aggregated DLIO trace data from a dftracer RocksDB. Opens the database
// read-only, iterates the AGGREGATION column family, and materializes per-rank
// trace arrays, distribution sample arrays, and trace-side
// ComponentTimeMetrics.
AggregatedTraces load_aggregated_traces(const std::string& db_path,
                                        const TraceLoaderOptions& options = {});

// Convenience: build a BarrierSimulatorContext from loaded traces.
BarrierSimulatorContext make_simulator_context(const AggregatedTraces& traces,
                                               int num_workers = 8,
                                               int prefetch_factor = 2);

}  // namespace dftracer::utils::utilities::dlio

#endif
