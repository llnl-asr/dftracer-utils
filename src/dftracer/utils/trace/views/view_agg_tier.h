#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGG_TIER_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGG_TIER_H

#include <dftracer/utils/trace/views/view_aggregate.h>

namespace dftracer::utils::trace::views::detail {

/// Answer a grouped aggregation directly from the index's aggregation tier (the
/// pre-folded per-key MetricStats), folding CF rows onto the plan's group_by.
/// Returns false with `out` untouched when the tier cannot answer the plan
/// exactly, so the caller scans instead. Event-exact: dur/size carry the stored
/// power sum (m2) as sumsq. `out` is the mergeable engine AggState (finalize
/// with finalize_engine_result). Requires plan.schema to be built.
bool agg_tier_collect(const ViewPlan& plan,
                      dftracer::utils::dataframe::AggStatePtr& out);

/// One-pass unified read of the EVENT and PROFILE maps from the aggregation
/// tier. Profiles are events with extra epoch/step key dims (grouped via Arg
/// keys) and share the dur/size value schema, so both fold into one
/// aggregation, then split by type into `out_regular` (events) and
/// `out_aggregated` (profiles) with no second scan. Returns false (outputs
/// untouched) when the tier cannot answer the plan exactly. Requires
/// plan.schema to be built.
// Shard range [shard_begin, shard_end); shard_end <= 0 means all shards.
bool events_profiles_collect(
    const ViewPlan& plan, dftracer::utils::dataframe::DataFrame& out_regular,
    dftracer::utils::dataframe::DataFrame& out_aggregated, int shard_begin = 0,
    int shard_end = 0, const ProgressFn* progress = nullptr);

/// One-pass read of the SYSTEM_METRICS CF (separate CF, its own key/value
/// format, per-metric named float stats). Each stored row is already aggregated
/// per (host_hash, name, time_bucket); emits one output row per key with a
/// dynamic column per metric (its mean), the sorted union of metric names
/// giving a stable schema. Absent metrics are NaN. Returns false (out_table
/// untouched) when there is no covering tier.
// Reads the SYSTEM_METRICS CF for shard range [shard_begin, shard_end)
// (shard_end <= 0 means all shards); the CF now carries the same shard prefix.
bool system_collect(const ViewPlan& plan,
                    dftracer::utils::dataframe::DataFrame& out_table,
                    int shard_begin = 0, int shard_end = 0,
                    const ProgressFn* progress = nullptr);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGG_TIER_H
