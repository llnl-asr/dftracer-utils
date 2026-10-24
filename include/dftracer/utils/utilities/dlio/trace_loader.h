#ifndef DFTRACER_UTILS_UTILITIES_DLIO_TRACE_LOADER_H
#define DFTRACER_UTILS_UTILITIES_DLIO_TRACE_LOADER_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/quantile_store.h>
#include <dftracer/utils/utilities/dlio/barrier_simulator.h>
#include <dftracer/utils/utilities/dlio/statistic.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::dlio {

using QuantileStore =
    ::dftracer::utils::index::schemas::dft::agg::QuantileStore;

/// Default DLIO category / event names; overridable per-component via
/// TraceLoaderOptions.
inline constexpr std::string_view CATEGORY_DATALOADER = "dataloader";
inline constexpr std::string_view CATEGORY_DATA = "data";
inline constexpr std::string_view EVENT_FETCH_BLOCK = "fetch.block";
inline constexpr std::string_view EVENT_FETCH_ITER = "fetch.iter";
inline constexpr std::string_view EVENT_PREPROCESS = "preprocess";
inline constexpr std::string_view EVENT_ITEM = "item";

/// A (category, name) pair selecting one DLIO component.
struct EventSelector {
    std::string cat;
    std::string name;
};

struct AggregatedTraces {
    /// Per-rank concatenated sample sequences (seconds), in pid-ascending,
    /// then time-bucket-ascending order.
    std::vector<std::vector<double>> fetch_block_trace;
    std::vector<std::vector<double>> fetch_iter_trace;
    std::vector<std::vector<double>> getitem_trace;

    /// Flat sample arrays for distribution fitting (seconds).
    std::vector<double> computation_times;  ///< fetch.block
    std::vector<double> preprocess_times;   ///< preprocess

    /// Duration quantiles merged across all (rank, bucket) entries for each
    /// component. Null when the component has no events.
    dstd::SharedPtr<QuantileStore> fetch_block_quantiles;
    dstd::SharedPtr<QuantileStore> fetch_iter_quantiles;
    dstd::SharedPtr<QuantileStore> preprocess_quantiles;
    dstd::SharedPtr<QuantileStore> getitem_quantiles;

    /// Statistics with min/max/mean/count populated, quantiles attached when
    /// present.
    Statistic fetch_block_stats;
    Statistic fetch_iter_stats;
    Statistic preprocess_stats;
    Statistic getitem_stats;

    /// Aggregate metrics (seconds).
    double trace_e2e_duration = 0.0;
    double trace_rank_variance = 0.0;
    std::vector<double> trace_per_rank_throughput;

    /// Per-component accumulated / union times. Both computed from actual
    /// (ts, te) boundaries in the CF rather than a fixed-ratio heuristic.
    ComponentTimeMetrics trace_preprocess_metrics;
    ComponentTimeMetrics trace_fetch_iter_metrics;
    ComponentTimeMetrics trace_fetch_block_metrics;

    /// Discovered PIDs (sorted) - index in this list defines rank ID.
    std::vector<std::uint64_t> rank_pids;

    int num_ranks = 0;
    int num_steps = 0;  ///< min length across ranks of fetch_block_trace

    /// True if any AGGREGATION entry was found.
    bool any_data = false;

    /// True if any component has duration quantiles. Drives whether sample
    /// synthesis uses inverse-CDF or mean replication.
    bool quantiles_available = false;

    /// Chunks the query read again from the trace. Zero when the stored
    /// partials answered every (pid, bucket, cat, name) group.
    std::uint64_t chunks_reread = 0;

    /// Time bucket width in microseconds (from the aggregation config).
    std::uint64_t time_interval_us = 0;
};

struct TraceLoaderOptions {
    /// Hard cap on samples synthesized per (cat, name, pid, bucket) entry, so a
    /// single high-count bucket cannot blow up memory. 0 disables the cap.
    std::uint64_t max_samples_per_entry = 100;
    /// Seed for inverse-CDF quantile sampling.
    std::uint64_t seed = 0xD15710;

    /// Per-component (cat, name) selectors; defaults are the DLIO event names.
    EventSelector fetch_block{std::string(CATEGORY_DATALOADER),
                              std::string(EVENT_FETCH_BLOCK)};
    EventSelector fetch_iter{std::string(CATEGORY_DATALOADER),
                             std::string(EVENT_FETCH_ITER)};
    EventSelector preprocess{std::string(CATEGORY_DATA),
                             std::string(EVENT_PREPROCESS)};
    EventSelector item{std::string(CATEGORY_DATA), std::string(EVENT_ITEM)};
};

/// Loads aggregated DLIO trace data with one aggregation query over `files`,
/// grouped by (pid, `config.time_interval_us` window, cat, name), and
/// materializes per-rank trace arrays, distribution sample arrays, and
/// trace-side ComponentTimeMetrics. `index_path` is the index store of `files`
/// and `config` the config it was built with. Blocks until the query ends.
/// Throws DFTUtilsException when the query fails.
AggregatedTraces load_aggregated_traces(
    const std::string& index_path, const dstd::Vector<dstd::String>& files,
    const index::schemas::dft::agg::AggregationConfig& config,
    const TraceLoaderOptions& options = {});

/// Overlays (cat, name) overrides from a YAML or JSON file onto the component
/// selectors in `options`. Top-level keys fetch_block, fetch_iter, preprocess
/// and item each take an optional `cat` and `name`; omitted keys and fields are
/// left untouched. Throws DFTUtilsException on a missing file or parse error.
void load_event_map(const std::string& path, TraceLoaderOptions& options);

/// Convenience: build a BarrierSimulatorContext from loaded traces.
BarrierSimulatorContext make_simulator_context(const AggregatedTraces& traces,
                                               int num_workers = 8,
                                               int prefetch_factor = 2);

}  // namespace dftracer::utils::utilities::dlio

#endif
