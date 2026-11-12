// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGG_PARTIALS_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGG_PARTIALS_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_plan.h>

namespace dftracer::utils::trace::views::detail {

/// Fills `out` with the mergeable engine AggState (finalize with
/// finalize_engine_result) from the partials of the files of `plan`. Returns
/// false, with `out` untouched, when the partials cannot answer it exactly and
/// the caller scans. Requires plan.schema. The error is what the index read or
/// run_agg_query reports.
coro::CoroTask<dstd::Result<bool>> partials_collect(
    const ViewPlan& plan, dftracer::utils::dataframe::AggStatePtr& out);

/// As partials_collect, but folds the events by the rules of the index fold, so
/// a missing, stale or unreadable index does not decline. Returns false, with
/// `out` untouched, only for a plan shape the partials cannot answer either.
/// Requires plan.schema. The error is what run_agg_query reports.
coro::CoroTask<dstd::Result<bool>> partials_scan_collect(
    const ViewPlan& plan, dftracer::utils::dataframe::AggStatePtr& out);

/// Event groups go to `out_regular`, non-sys counter groups to
/// `out_aggregated`. False leaves both untouched. Keeps keys hashing into
/// [shard_begin, shard_end) of AGG_KEY_NUM_SHARDS, all when shard_end <= 0.
/// `progress` gets (0, shards) then (shards, shards). Requires plan.schema.
coro::CoroTask<dstd::Result<bool>> partials_collect_typed(
    const ViewPlan& plan, dftracer::utils::dataframe::DataFrame& out_regular,
    dftracer::utils::dataframe::DataFrame& out_aggregated, int shard_begin = 0,
    int shard_end = 0, const ProgressFn* progress = nullptr);

/// The query the index rules fold the events of `plan` by on a scan. Empty
/// when the plan stays on the frame path. Reads no trace; the error is that of
/// an index that cannot be read.
dstd::Result<dstd::Optional<AggQuery>> trace_fold_query(const ViewPlan& plan);

/// The engine state of `rows`. `texts` holds the key texts the rows name by id.
dftracer::utils::dataframe::AggStatePtr seed_rows(
    const ViewPlan& plan, AggInputSpec spec, const AggQuery& query,
    const dftracer::utils::dstd::StringIntern& texts,
    const AggQueryResult::Rows& rows);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGG_PARTIALS_H
