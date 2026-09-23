#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGGREGATE_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGGREGATE_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_resolver.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// The query-derived aggregation schema (make_agg_schema/AggSchema) plus the
// group-key/column-name and name-resolution helpers shared by the engine
// aggregation path, the tier, and result post-processing.
namespace dftracer::utils::trace::views::detail {

// Unit separator between composite group-key parts (never appears in field
// values, so it round-trips a multi-column key as one map key).
inline constexpr char GROUP_SEP = '\x1f';

// Separator joining a SetUnion group's distinct values into one text-column
// cell; a control char so it does not collide with categorical values.
inline constexpr char SET_SEP = '\x1e';

// The per-field aggregation atom (shared with the aggregation tier). See
// dataframe/field_stat.h.
using dftracer::utils::dataframe::FieldStat;

// Query-derived fold schema: the distinct agg fields and how each AggSpec maps
// onto them. A pure function of the plan, built once per terminal (cached on
// ViewPlan) so the per-event fold touches each field once, not once per spec.
struct AggSchema {
    std::vector<std::string> fields;  // distinct fields to accumulate
    std::vector<int> spec_field;      // plan.agg[i] -> fields index, or -1
    std::vector<int> spec_argmax;     // plan.agg[i] -> argmax slot, or -1
    std::size_t argmax_count = 0;
    std::vector<bool> field_scaled;   // fields[i] is ts/dur/te (time_scale)
    // fields[i] -> DDSketch slot for percentile/histogram aggs, or -1.
    std::vector<int> field_sketch;
    std::size_t sketch_count = 0;
    std::vector<int> spec_set;      // plan.agg[i] -> SetUnion slot, or -1
    std::size_t set_count = 0;
    bool want_occupancy = false;    // any busy/concurrency/utilization spec
    std::uint64_t occ_cell_us = 0;  // optional exact-union tolerance; 0 = exact
    // A per-arg DDSketch is collected only when a numeric_arg_aggs reduction
    // needs quantiles (Pct); otherwise the dyn FieldStat alone serves the
    // reductions and no sketch is allocated.
    bool dyn_sketch = false;
};

// Build the fold schema; ensure_schema memoizes it on the plan. Call
// ensure_schema at a terminal's single-threaded entry, before the parallel
// fold, so worker reads of plan.schema never race the build.
AggSchema make_agg_schema(const ViewPlan& plan);
const AggSchema& ensure_schema(const ViewPlan& plan);

// Lazily build (and cache on the plan) the index-backed name resolver; null
// when the plan has no resolved-name group key.
const GroupResolver* ensure_resolver(const ViewPlan& plan);

// Feed pid -> rank harvested from PR metadata into the plan's resolver so the
// Rank group key relabels pid groups post-aggregation. Empties `ranks`.
void apply_ranks(const ViewPlan& plan,
                 std::unordered_map<std::uint64_t, std::string>& ranks);

// Resolve one group value from its stored hash to the name for `kind`
// (FilePath/FileName/HostName); returns `hash` unchanged for other kinds.
std::string resolve_group_value(const GroupResolver& r, GroupKey::Kind kind,
                                const std::string& hash);

// Apply `gk`'s value transform to an already-resolved group value. Coarsens
// the grain, so both the scan and the tier must call it or the same query
// answers differently depending on which path served it.
std::string apply_group_transform(const GroupKey& gk, std::string v);

// Column names for group-key and aggregate output columns.
std::string group_col_name(const GroupKey& gk);
std::string agg_col_name(const AggSpec& spec);

// Column name for one per-arg reduction of the dyn (auto_numeric_metrics) path:
// "<op>_<arg>" (Pct uses the spec's out_name prefix). The legacy bare-mean
// column keeps the bare arg name and does not go through here.
std::string dyn_col_name(const AggSpec& spec, const std::string& key);

// Drop every column whose name is not in `select`, preserving result order. A
// no-op when `select` is empty or names a column that is not present. Apply
// only at the final collect() boundary, never to an intermediate that will be
// merged further.
void project_columns(dftracer::utils::dataframe::DataFrame& batch,
                     const std::vector<std::string>& select);

// The plan's sort_by/topk/offset+limit/select, applied in that order. Apply
// once at the final result boundary, after all merges.
dftracer::utils::dataframe::DataFrame apply_agg_post_ops(
    dftracer::utils::dataframe::DataFrame batch, const ViewPlan& plan);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGGREGATE_H
