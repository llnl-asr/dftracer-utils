// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_SYSTEM_COLLECT_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_SYSTEM_COLLECT_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan.h>

namespace dftracer::utils::trace::views::detail {

/// One row per (host_hash, name, time_bucket) of the sys counters, with a
/// column per metric (its mean, NaN when absent), in the shard range
/// [shard_begin, shard_end) (shard_end <= 0 means all). Returns false, with
/// out_table untouched, unless the plan spans the files of one current index.
bool system_collect(const ViewPlan& plan,
                    dftracer::utils::dataframe::DataFrame& out_table,
                    int shard_begin = 0, int shard_end = 0,
                    const ProgressFn* progress = nullptr);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_SYSTEM_COLLECT_H
