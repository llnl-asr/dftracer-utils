#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLAN_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLAN_H

#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/views/view.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::indexing {
class BloomFilterCache;
}

namespace dftracer::utils::trace::views::detail {

/// Query-derived fold schema (distinct agg fields + per-spec mapping), a pure
/// function of the plan. Cached lazily on the plan so the per-event fold builds
/// it once, not per event. Defined in view_aggregate.h.
struct AggSchema;

/// Index-backed name maps for resolved-name group keys. Defined in
/// view_resolver.h.
class GroupResolver;

/// The logical plan a `View` carries. The ops form a linear pipeline, so a flat
/// struct captures it; each builder copies and mutates one field.
struct ViewPlan {
    std::vector<ViewFile> files;
    indexing::BloomFilterCache* bloom_cache = nullptr;

    std::optional<query::Query> query;
    std::optional<std::pair<double, double>> time_range;
    // Default spans all phases so a naive View sees every event; callers that
    // want only ph="X" events (or only ph="C") say so with .phase().
    Phase phase = Phase::Any;
    bool include_metadata = true;
    bool emit_all_metadata = false;  // harvest every hash-metadata record

    std::uint64_t time_bucket_us = 0;
    /// Bucket alignment origin (in the post-time_scale unit): bucket i spans
    /// [origin + i*width, origin + (i+1)*width). 0 = absolute (aligned to 0).
    std::uint64_t bucket_origin_us = 0;
    /// When set, align buckets to the trace's minimum timestamp (read cheaply
    /// from the index zone maps, no scan) instead of 0 or a fixed origin; the
    /// executor resolves it into bucket_origin_us before the fold.
    bool bucket_origin_min = false;
    /// Optional tolerance on the occupancy interval union, microseconds; event
    /// endpoints snap to this grid to bound memory at a small over-estimate.
    /// 0 = exact union (default).
    std::uint64_t occ_cell_us = 0;
    /// Multiply ts/dur/te by this to normalize from the trace's native time
    /// unit to a target (source_ns / target_ns; 1.0 = no scaling). Applied
    /// before time_bucket so bucketing is in the target unit.
    double time_scale = 1.0;
    std::vector<GroupKey> group_by;
    std::vector<AggSpec> agg;
    /// Aggregate every numeric args.* field as a dynamic per-group metric
    /// (skipping metadata/pre-aggregated args). This is how ph="C" counters
    /// aggregate without naming the fields up front. With `numeric_arg_aggs`
    /// empty it emits one bare-named per-arg mean column (legacy). With
    /// reductions listed, it emits one `<op>_<arg>` column per (arg,
    /// reduction).
    bool auto_numeric_metrics = false;
    /// Reductions applied to every discovered numeric arg (op + q; the AggSpec
    /// field is ignored, the reduction runs on each arg key). Empty = the
    /// legacy bare-mean column. Only FieldStat-derivable ops
    /// (Count/Sum/Min/Max/SumSq/ Mean/Var/Std/Skew/Kurt) are supported here;
    /// Pct needs a per-arg sketch.
    std::vector<AggSpec> numeric_arg_aggs;
    /// Out-of-core aggregation budget shared by collect / export_counters /
    /// aggregate_partial: when a worker's in-memory group map grows past this
    /// many bytes it spills to a sorted temp run; the runs are k-way merged at
    /// the end, so the scan/merge peak stays bounded regardless of group
    /// cardinality. 0 (the default) means "auto": ~1/3 of available memory (see
    /// resolve_spill_budget). Set NO_SPILL_BUDGET to disable spilling.
    /// collect() still materializes the final table, so this bounds
    /// intermediate memory only.
    std::uint64_t memory_budget = 0;
    std::vector<std::string> select;

    /// Pagination. `limit` caps the output (0 = unlimited); `offset` skips that
    /// many leading rows/events first. Applied to collect()'s materialized rows
    /// and to the streamed event output. Under a parallel scan the export order
    /// is scan-order, so a stable page needs a deterministic sort first.
    std::uint64_t limit = 0;
    std::uint64_t offset = 0;

    /// Post-aggregation ordering of collect()'s result rows, applied before
    /// offset/limit with the vec sort kernels: `sort_col` sorts by that column
    /// (descending if `sort_desc`); `topk_col` then keeps the `topk_k` best
    /// rows (largest unless `!topk_largest`). Empty column = not applied.
    std::string sort_col;
    bool sort_desc = false;
    std::string topk_col;
    std::int64_t topk_k = -1;
    bool topk_largest = true;

    /// Root for the materialized-view cache (persist/reconstruct). Empty =
    /// derive from the files' index location. The cache dir is
    /// `<root>/<plan-signature>`, so a changed grain (files, filter, group_by,
    /// agg, bucket, range) lands in a different dir and recomputes.
    std::string rollup_root;

    /// Directory holding materialized filtered-trace views. Empty = derive as
    /// `<parent-of-index>/.dftindex-views`. Set to relocate MVs (e.g. a scratch
    /// dir) away from the dataset.
    std::string views_root;

    /// Persist this query's result as a materialized view (rollup) as a
    /// byproduct of answering it, so a later matching query reads it back.
    /// Opt-in via View::materialize() - a plain collect() never persists.
    bool materialize = false;

    /// Row-MV materialize granularity (bytes): `mv_checkpoint_size` is the
    /// index checkpoint size (one gzip member per checkpoint, so it drives
    /// intra-file read parallelism); `mv_part_size` is the uncompressed size at
    /// which the write rolls to a new part file. 0 = engine defaults.
    std::uint64_t mv_checkpoint_size = 0;
    std::uint64_t mv_part_size = 0;

    /// Cooperative cancellation, polled by every terminal at its loop
    /// boundaries; empty = never cancelled.
    std::function<bool()> cancelled;

    /// Memoized fold schema (ensure_schema builds it once before the parallel
    /// fold; workers read it race-free). Mutable so a const plan can cache it;
    /// shared_ptr so plan copies share one build.
    mutable std::shared_ptr<const AggSchema> schema;

    /// Index-backed name maps, built lazily by ensure_resolver when the plan
    /// has a FilePath/HostName group key; used by the post-aggregation re-key
    /// pass.
    mutable std::shared_ptr<const GroupResolver> resolver;
};

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLAN_H
