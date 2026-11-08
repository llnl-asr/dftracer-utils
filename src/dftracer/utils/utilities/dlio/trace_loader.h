// SPDX-License-Identifier: MIT
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

namespace dftracer::utils::utilities::dlio {

using QuantileStore =
    ::dftracer::utils::index::schemas::dft::agg::QuantileStore;

/// Default DLIO category / event names; overridable per-component via
/// @ref TraceLoaderOptions.
inline constexpr dstd::StringView CATEGORY_DATALOADER = "dataloader";
inline constexpr dstd::StringView CATEGORY_DATA = "data";
inline constexpr dstd::StringView EVENT_FETCH_BLOCK = "fetch.block";
inline constexpr dstd::StringView EVENT_FETCH_ITER = "fetch.iter";
inline constexpr dstd::StringView EVENT_PREPROCESS = "preprocess";
inline constexpr dstd::StringView EVENT_ITEM = "item";

/// A (category, name) pair selecting one DLIO component.
struct EventSelector {
    dstd::String cat;
    dstd::String name;
};

struct AggregatedTraces {
    /// Per-rank concatenated sample sequences (seconds), in pid-ascending,
    /// then time-bucket-ascending order.
    dstd::Vector<dstd::Vector<double>> fetch_block_trace;
    dstd::Vector<dstd::Vector<double>> fetch_iter_trace;
    dstd::Vector<dstd::Vector<double>> getitem_trace;

    /// Flat sample arrays for distribution fitting (seconds).
    dstd::Vector<double> computation_times;  ///< fetch.block
    dstd::Vector<double> preprocess_times;   ///< preprocess

    /// Duration quantiles merged across all (rank, bucket) entries for each
    /// component. Null when the component has no events.
    dstd::SharedPtr<QuantileStore> fetch_block_quantiles;
    dstd::SharedPtr<QuantileStore> fetch_iter_quantiles;
    dstd::SharedPtr<QuantileStore> preprocess_quantiles;
    dstd::SharedPtr<QuantileStore> getitem_quantiles;

    Statistic fetch_block_stats;
    Statistic fetch_iter_stats;
    Statistic preprocess_stats;
    Statistic getitem_stats;

    /// Aggregate metrics (seconds).
    double trace_e2e_duration = 0.0;
    double trace_rank_variance = 0.0;
    dstd::Vector<double> trace_per_rank_throughput;

    /// Per-component accumulated / union times. Both computed from actual
    /// (ts, te) boundaries in the CF rather than a fixed-ratio heuristic.
    ComponentTimeMetrics trace_preprocess_metrics;
    ComponentTimeMetrics trace_fetch_iter_metrics;
    ComponentTimeMetrics trace_fetch_block_metrics;

    /// Discovered PIDs (sorted) - index in this list defines rank ID.
    dstd::Vector<std::uint64_t> rank_pids;

    int rank_count = 0;
    int step_count = 0;  ///< min length across ranks of fetch_block_trace

    /// True if any AGGREGATION entry was found.
    bool any_data = false;

    /// True if any component has duration quantiles.
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
    EventSelector fetch_block{dstd::String(CATEGORY_DATALOADER),
                              dstd::String(EVENT_FETCH_BLOCK)};
    EventSelector fetch_iter{dstd::String(CATEGORY_DATALOADER),
                             dstd::String(EVENT_FETCH_ITER)};
    EventSelector preprocess{dstd::String(CATEGORY_DATA),
                             dstd::String(EVENT_PREPROCESS)};
    EventSelector item{dstd::String(CATEGORY_DATA), dstd::String(EVENT_ITEM)};
};

/// Loads aggregated DLIO trace data with one aggregation query over `files`,
/// grouped by (pid, `config.time_interval_us` window, cat, name). `index_path`
/// is the index store of `files` and `config` the config it was built with.
/// Blocks until the query ends and throws @ref
/// dftracer::utils::common::DFTUtilsException "DFTUtilsException" on failure.
AggregatedTraces load_aggregated_traces(
    dstd::StringView index_path, const dstd::Vector<dstd::String>& files,
    const index::schemas::dft::agg::AggregationConfig& config,
    const TraceLoaderOptions& options = {});

/// Overlays (cat, name) overrides from a YAML or JSON file onto the component
/// selectors in `options`. Top-level keys fetch_block, fetch_iter, preprocess
/// and item each take an optional `cat` and `name`; omitted keys and fields are
/// left untouched. Throws @ref dftracer::utils::common::DFTUtilsException
/// "DFTUtilsException" on a missing file or parse error.
void load_event_map(dstd::StringView path, TraceLoaderOptions& options);

/// Convenience: build a @ref BarrierSimulatorContext from loaded traces.
BarrierSimulatorContext make_simulator_context(const AggregatedTraces& traces,
                                               int worker_count = 8,
                                               int prefetch_factor = 2);

}  // namespace dftracer::utils::utilities::dlio

#endif
