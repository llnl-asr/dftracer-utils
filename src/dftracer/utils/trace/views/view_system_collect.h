#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_SYSTEM_COLLECT_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_SYSTEM_COLLECT_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan.h>

namespace dftracer::utils::trace::views::detail {

/// One-pass read of the sys counters of the aggregation tier's column blocks,
/// each block holding the rows of one chunk with their exact arg values. Emits
/// one output row per (host_hash, name, time_bucket) with a dynamic column per
/// metric (its mean), the sorted union of metric names giving a stable schema
/// and absent metrics NaN. Reads the shard range [shard_begin, shard_end)
/// (shard_end <= 0 means all shards). Returns false (out_table untouched) when
/// the plan does not span the files of one index whose aggregation data is
/// current, or when a block that holds these rows dropped one of their args;
/// system_scan_collect then answers the same rows from the trace.
bool system_collect(const ViewPlan& plan,
                    dftracer::utils::dataframe::DataFrame& out_table,
                    int shard_begin = 0, int shard_end = 0,
                    const ProgressFn* progress = nullptr);

/// The same counters frame, read from the trace itself: the sys counters of the
/// plan's files grouped as system_collect groups them, for a plan whose column
/// blocks left a metric of those rows out. Returns false (out_table untouched)
/// when the plan does not span the files of one current aggregation tier, so
/// the bucket of a group is unknown.
coro::CoroTask<bool> system_scan_collect(
    const ViewPlan& plan, dftracer::utils::dataframe::DataFrame& out_table,
    int shard_begin = 0, int shard_end = 0,
    const ProgressFn* progress = nullptr);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_SYSTEM_COLLECT_H
