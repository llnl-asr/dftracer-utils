#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_SYSTEM_COLLECT_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_SYSTEM_COLLECT_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan.h>

namespace dftracer::utils::trace::views::detail {

/// One-pass read of the SYSTEM_METRICS CF (its own key/value format, per-metric
/// named float stats). Each stored row is already aggregated per (host_hash,
/// name, time_bucket); emits one output row per key with a dynamic column per
/// metric (its mean), the sorted union of metric names giving a stable schema.
/// Absent metrics are NaN. Reads the shard range [shard_begin, shard_end)
/// (shard_end <= 0 means all shards). Returns false (out_table untouched) when
/// the plan does not span the files of one index whose partials are current.
bool system_collect(const ViewPlan& plan,
                    dftracer::utils::dataframe::DataFrame& out_table,
                    int shard_begin = 0, int shard_end = 0,
                    const ProgressFn* progress = nullptr);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_SYSTEM_COLLECT_H
