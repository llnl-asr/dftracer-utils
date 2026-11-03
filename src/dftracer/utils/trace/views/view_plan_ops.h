// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLAN_OPS_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLAN_OPS_H

#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/views/batch_bridge.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <cstdint>
#include <utility>

// The trace scan as functions on an immutable ViewPlan: builders return a new
// plan with one setting changed, terminals execute a plan. View, ViewSource,
// ViewSession and the C ABI build and run their plans through these. A
// terminal takes its plan by value, so the task owns it.
namespace dftracer::utils::trace::views::detail::scan {

using detail::ScanPlan;

ScanPlan from_files(dstd::Vector<ViewFile> files);
ScanPlan from_file(dstd::String file_path, dstd::String index_path = "");
coro::CoroTask<ScanPlan> from_directory(dstd::String dir,
                                        dstd::String index_path = "");
dstd::Vector<TraceConfig> config(const ScanPlan& plan_);
dstd::Vector<dstd::String> columns(const ScanPlan& plan_);
dstd::Vector<ColumnInfo> schema(const ScanPlan& plan_);
/// Writes the path catalog of each file of `plan_` that has no current one,
/// by one unfiltered scan of those files only. A catalog that cannot be
/// written (locked index) stays absent.
void ensure_catalog(const ScanPlan& plan_);
/// The leaves of the catalog, built first where missing.
dstd::Vector<SchemaLeaf> schema_tree(const ScanPlan& plan_);
/// The leaves of the catalogs as they stand; scans nothing.
dstd::Vector<SchemaLeaf> catalog_tree(const ScanPlan& plan_);
/// The first file of the plan whose index holds no path catalog (no index,
/// or one built without its statistics tier), or nullopt.
dstd::Optional<dstd::String> file_without_catalog(const ScanPlan& plan_);
/// Each column's type from the index; a column of mixed JSON types is a JSON
/// String (DataType::json).
dstd::HashMap<dstd::String, dataframe::DataType> column_types(
    const ScanPlan& plan_);
/// The columns `plan_` gives as JSON (see detail::JsonColumns); null when none.
dstd::SharedPtr<const detail::JsonColumns> json_columns(
    const detail::ViewPlan& plan);
TimeMetric time_metric(const ScanPlan& plan_);
ScanPlan filter(const ScanPlan& plan_, Query q);
ScanPlan phase(const ScanPlan& plan_, Phase p);
ScanPlan time_range(const ScanPlan& plan_, double begin, double end);
ScanPlan time_bucket(const ScanPlan& plan_, std::uint64_t interval_us);
ScanPlan time_bucket(const ScanPlan& plan_, std::uint64_t interval_us,
                     std::uint64_t origin_us);
ScanPlan time_bucket_min(const ScanPlan& plan_, std::uint64_t interval_us);
ScanPlan occ_cell(const ScanPlan& plan_, std::uint64_t cell_us);
ScanPlan time_scale(const ScanPlan& plan_, double ns_ratio);
ScanPlan group_by(const ScanPlan& plan_, dstd::Vector<GroupKey> keys);
ScanPlan agg(const ScanPlan& plan_, dstd::Vector<AggSpec> specs);
ScanPlan agg(const ScanPlan& plan_, dstd::Vector<FieldAggExpr> exprs);
ScanPlan agg_numeric_args(const ScanPlan& plan_);
ScanPlan agg_numeric_args(const ScanPlan& plan_,
                          dstd::Vector<AggSpec> reductions);
ScanPlan memory_budget(const ScanPlan& plan_, std::uint64_t bytes);
ScanPlan count_scans(const ScanPlan& plan_,
                     dstd::SharedPtr<ScanCounter> counter);
ScanPlan auto_spill(const ScanPlan& plan_);
ScanPlan select(const ScanPlan& plan_, dstd::Vector<dstd::String> cols);
ScanPlan name_columns(const ScanPlan& plan_,
                      dstd::Vector<dataframe::Field> columns);
ScanPlan limit(const ScanPlan& plan_, std::uint64_t n);
ScanPlan offset(const ScanPlan& plan_, std::uint64_t n);
ScanPlan sort_by(const ScanPlan& plan_, dstd::String column,
                 bool descending = false);
ScanPlan topk(const ScanPlan& plan_, dstd::String column, std::int64_t k,
              bool largest = true);
ScanPlan materialize(const ScanPlan& plan_, std::uint64_t checkpoint_size,
                     std::uint64_t part_size);
/// The plan read by a trace operation: records without a time are skipped.
ScanPlan timed(const ScanPlan& plan_);
ScanPlan record_schema(const ScanPlan& plan_, const dstd::String& id);
ScanPlan cancel_when(const ScanPlan& plan_, dstd::Function<bool()> pred);
ScanPlan ordered(const ScanPlan& plan_);
ScanPlan rollup_root(const ScanPlan& plan_, dstd::String dir);
ScanPlan views_root(const ScanPlan& plan_, dstd::String dir);
coro::CoroTask<ExportStats> for_each_batch(
    ScanPlan plan_,
    dstd::Function<void(std::size_t, const dstd::Vector<dstd::StringView>&)>
        on_batch,
    std::size_t num_slots, std::uint64_t limit = 0);
bool export_would_bootstrap(const ScanPlan& plan_);
bool collect_would_bootstrap(const ScanPlan& plan_);
coro::CoroTask<ExportStats> export_json(ScanPlan plan_, ExportSink& sink);
coro::CoroTask<ExportStats> export_trace(ScanPlan plan_,
                                         TraceWriteOptions opts);
dataframe::LazyFrame collect(const ScanPlan& plan_);
coro::AsyncGenerator<dataframe::DataFrame> stream(ScanPlan plan_,
                                                  std::int64_t morsel_rows = 0);
coro::CoroTask<dataframe::DataFrame> collect_frame(ScanPlan plan_);
bool is_row_query(const ScanPlan& plan_);
coro::CoroTask<dataframe::DataFrame> call_tree(
    ScanPlan plan_, dstd::Vector<dstd::String> partition, dstd::String ts,
    dstd::String dur, dstd::String name);
coro::CoroTask<dataframe::DataFrame> flamegraph(
    ScanPlan plan_, dstd::Vector<dstd::String> partition, dstd::String ts,
    dstd::String dur, dstd::String name, dstd::Vector<dstd::String> group);
coro::CoroTask<dstd::Pair<dataframe::DataFrame, dataframe::DataFrame>>
containment(ScanPlan plan_, dstd::Vector<dstd::String> partition,
            dstd::String ts, dstd::String dur, dstd::String name,
            dstd::Vector<dstd::String> group);
coro::CoroTask<dstd::String> flamegraph_partial(
    ScanPlan plan_, dstd::Vector<dstd::String> partition, dstd::String ts,
    dstd::String dur, dstd::String name, dstd::Vector<dstd::String> group);
dataframe::DataFrame merge_flamegraph_partials(
    const dstd::Vector<dstd::StringView>& partials);
coro::CoroTask<ExportStats> run_folds(
    ScanPlan plan_, dstd::Span<detail::Fold* const> folds,
    dftracer::utils::dstd::StringIntern& intern,
    detail::DynamicPrune* dyn_prune = nullptr,
    coro::CoroSemaphore* gate = nullptr);
coro::CoroTask<ExportStats> run(ScanPlan plan_,
                                const ProgressFn* progress = nullptr);
dstd::String materialize_dir(const ScanPlan& plan_);
void register_materialized(const ScanPlan& plan_, const dstd::String& dir);
dstd::Vector<dstd::String> mv_source(const ScanPlan& plan_);
coro::CoroTask<TypedResult> collect_typed(ScanPlan plan_, int shard_begin,
                                          int shard_end,
                                          const ProgressFn* progress = nullptr);
coro::CoroTask<ExportStats> export_counters(ScanPlan plan_, ExportSink& sink);
coro::CoroTask<void> materialize_partials(
    ScanPlan plan_, const dstd::Vector<dstd::StringView>& partials);
dstd::Optional<dataframe::DataFrame> reconstruct_if_cached(
    const ScanPlan& plan_);
coro::CoroTask<dstd::String> aggregate_partial(ScanPlan plan_);
dataframe::DataFrame merge_partials_to_table(
    const ScanPlan& plan_, const dstd::Vector<dstd::StringView>& partials);
ExportStats merge_counter_partials(
    const ScanPlan& plan_, const dstd::Vector<dstd::StringView>& partials,
    ExportSink& sink);

}  // namespace dftracer::utils::trace::views::detail::scan

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLAN_OPS_H
