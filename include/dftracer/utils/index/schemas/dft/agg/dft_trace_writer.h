#ifndef DFTRACER_UTILS_TRACE_AGGREGATORS_PERFETTO_TRACE_WRITER_UTILITY_H
#define DFTRACER_UTILS_TRACE_AGGREGATORS_PERFETTO_TRACE_WRITER_UTILITY_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_key.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_metrics.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_output.h>
#include <dftracer/utils/index/schemas/dft/agg/association_tracker.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::index::schemas::dft::agg {

class EventAggregator;

/// How the aggregator's folded records are rendered on output. AGGREGATED
/// writes them as ph:3 AGGREGATED records (the new format's folded-event
/// phase); COUNTER as ph:2 counter samples; REGULAR expands them back to ph:1
/// COMPLETE events.
enum class TraceEventFormat { AGGREGATED, COUNTER, REGULAR };

struct DftracerTraceWriterInput {
    std::string output_path;
    const EventAggregator* aggregator = nullptr;
    const AssociationTracker* tracker = nullptr;
    const AggregationConfig* agg_config = nullptr;
    std::unique_ptr<AssociationTracker> owned_tracker;
    ankerl::unordered_dense::set<std::uint64_t> root_pids;
    std::uint64_t trace_duration = 0;
    BoundaryTimeRangesMap boundary_ranges;
    bool compute_statistics = true;
    bool compute_percentiles = false;
    std::vector<double> percentiles;
    bool compress = false;
    int compression_level = 6;
    TraceEventFormat format = TraceEventFormat::AGGREGATED;
    /// Workers add their per-shard key count here if non-null.
    std::atomic<std::size_t>* keys_written = nullptr;
    /// Total shard-prefix range (half-open) this invocation is responsible
    /// for. Defaults cover the whole key space. MPI drivers set a disjoint
    /// range per rank so N ranks collectively cover `[0, AGG_KEY_NUM_SHARDS)`
    /// without overlap. Local coroutine workers within a single process
    /// further subdivide this range.
    std::uint16_t shard_begin = 0;
    std::uint16_t shard_end = 0;  ///< 0 means "use AGG_KEY_NUM_SHARDS"
    /// Emit the JSON array prologue (`[\n` + trace_metadata + root_process
    /// markers) to this invocation's output. MPI drivers set this on rank 0
    /// only so concatenated rank outputs produce exactly one array open.
    bool emit_header = true;
    /// Emit the JSON array epilogue (`]`). MPI drivers set this on the last
    /// rank only.
    bool emit_footer = true;
};

using DftracerTraceWriterOutput = bool;

class DftTraceWriter {
   public:
    coro::CoroTask<bool> operator()(
        CoroScope& ctx, const DftracerTraceWriterInput& input) const;

    /// Scope-less overload: opens its own CoroScope on the current executor.
    /// Takes input by value since it is move-only.
    coro::CoroTask<bool> operator()(DftracerTraceWriterInput input) const {
        return with_scope(*this, std::move(input));
    }
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_TRACE_AGGREGATORS_PERFETTO_TRACE_WRITER_UTILITY_H
