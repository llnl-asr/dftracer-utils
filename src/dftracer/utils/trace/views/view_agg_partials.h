#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGG_PARTIALS_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGG_PARTIALS_H

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan.h>

// The view's aggregate, events and profiles reads over the chunk partials of
// the aggregation tier, through run_agg_query (trace/agg_query.h). A partial
// answers a group-by on category, name, host, and on pid and tid when the
// chunk holds one; what a partial cannot answer is re-read from its chunk, so
// a plan that needs that needs the traces. A path-decoded plan reads no chunk:
// it is declined when a re-read would be needed.
namespace dftracer::utils::trace::views::detail {

/// Answers the aggregation of `plan` from the partials of its files and fills
/// `out` with the mergeable engine AggState (finalize with
/// finalize_engine_result). Returns false, with `out` untouched, when the
/// partials cannot answer the plan exactly (no usable partials for a file, a
/// filter, field or aggregation they cannot hold, a time scale or window they
/// cannot serve); the caller scans then. Requires plan.schema to be built.
/// Throws what run_agg_query reports.
coro::CoroTask<bool> partials_collect(
    const ViewPlan& plan, dftracer::utils::dataframe::AggStatePtr& out);

/// The events and profiles of `plan` in one pass: `out_regular` holds the
/// groups of the events and `out_aggregated` those of the profiles (counters
/// of a category other than sys), each keyed [time_bucket?, group_by...] and
/// finalized like a fresh scan. The plan's time_bucket is the window of the
/// query. Returns false, with the outputs untouched, as partials_collect does.
///
/// A shard range [shard_begin, shard_end) of AGG_KEY_NUM_SHARDS keeps the
/// groups whose key hashes into it (shard_end <= 0 keeps all), so disjoint
/// ranges give disjoint groups that concatenate. Every range reads the same
/// partials. `progress` is called with (0, shards) and (shards, shards).
/// Requires plan.schema to be built.
coro::CoroTask<bool> partials_collect_typed(
    const ViewPlan& plan, dftracer::utils::dataframe::DataFrame& out_regular,
    dftracer::utils::dataframe::DataFrame& out_aggregated, int shard_begin = 0,
    int shard_end = 0, const ProgressFn* progress = nullptr);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGG_PARTIALS_H
