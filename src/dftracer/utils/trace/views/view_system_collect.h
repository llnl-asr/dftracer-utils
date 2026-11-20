// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_SYSTEM_COLLECT_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_SYSTEM_COLLECT_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan.h>

namespace dftracer::utils::trace::views::detail {

/// One row per (host_hash, name, time_bucket) of the sys counters, with a
/// column per metric (its mean, NaN when absent), in the shard range
/// [shard_begin, shard_end) (shard_end <= 0 means all). The plan may span the
/// files of several index roots, and some of the files of each. Returns false,
/// with out_table untouched, and the reason in `why` when it is given, unless
/// every file is in its index with current aggregation data and the indexes
/// share one interval. The error is that of a chunk column that cannot be
/// read.
dstd::Result<bool> system_collect(
    const ViewPlan& plan, dftracer::utils::dataframe::DataFrame& out_table,
    int shard_begin = 0, int shard_end = 0,
    const ProgressFn* progress = nullptr, dstd::String* why = nullptr);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_SYSTEM_COLLECT_H
