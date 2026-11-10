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

/// A plan builder that cannot take its argument (an index it cannot read, a
/// group key the schema lacks) returns a plan that holds the error
/// (@ref ViewPlan::error); every terminal on it returns that error.
ScanPlan from_files(dstd::Vector<ViewFile> files);
ScanPlan from_file(dstd::String file_path, dstd::String index_path = "");
coro::CoroTask<ScanPlan> from_directory(dstd::String dir,
                                        dstd::String index_path = "");
dstd::Result<dstd::Vector<TraceConfig>> config(const ScanPlan& plan);
dstd::Result<dstd::Vector<dstd::String>> columns(const ScanPlan& plan);
dstd::Result<dstd::Vector<ColumnInfo>> schema(const ScanPlan& plan);
/// Writes the path catalog of each file of `plan` that has no current one,
/// by one unfiltered scan of those files only. A catalog that cannot be
/// written (locked index) stays absent.
dstd::Result<void> ensure_catalog(const ScanPlan& plan);
/// The leaves of the catalog, built first where missing.
dstd::Result<dstd::Vector<SchemaLeaf>> schema_tree(const ScanPlan& plan);
/// The leaves of the catalogs as they stand; scans nothing.
dstd::Result<dstd::Vector<SchemaLeaf>> catalog_tree(const ScanPlan& plan);
/// The first file of the plan whose index holds no path catalog (no index,
/// or one built without its statistics tier), or nullopt.
dstd::Result<dstd::Optional<dstd::String>> file_without_catalog(
    const ScanPlan& plan);
/// Each column's type from the index; a column of mixed JSON types is a JSON
/// String (DataType::json).
dstd::Result<dstd::HashMap<dstd::String, dataframe::DataType>> column_types(
    const ScanPlan& plan);
/// The columns `plan` gives as JSON (see detail::JsonColumns); null when none.
dstd::Result<dstd::SharedPtr<const detail::JsonColumns>> json_columns(
    const detail::ViewPlan& plan);
TimeMetric time_metric(const ScanPlan& plan);
ScanPlan filter(const ScanPlan& plan, Query q);
ScanPlan phase(const ScanPlan& plan, Phase p);
ScanPlan time_range(const ScanPlan& plan, double begin, double end);
ScanPlan time_bucket(const ScanPlan& plan, std::uint64_t interval_us);
ScanPlan time_bucket(const ScanPlan& plan, std::uint64_t interval_us,
                     std::uint64_t origin_us);
ScanPlan time_bucket_min(const ScanPlan& plan, std::uint64_t interval_us);
ScanPlan occ_cell(const ScanPlan& plan, std::uint64_t cell_us);
ScanPlan time_scale(const ScanPlan& plan, double ns_ratio);
ScanPlan group_by(const ScanPlan& plan, dstd::Vector<GroupKey> keys);
ScanPlan agg(const ScanPlan& plan, dstd::Vector<AggSpec> specs);
ScanPlan agg(const ScanPlan& plan, dstd::Vector<FieldAggExpr> exprs);
ScanPlan agg_numeric_args(const ScanPlan& plan);
ScanPlan agg_numeric_args(const ScanPlan& plan,
                          dstd::Vector<AggSpec> reductions);
ScanPlan memory_budget(const ScanPlan& plan, std::uint64_t bytes);
ScanPlan count_scans(const ScanPlan& plan,
                     dstd::SharedPtr<ScanCounter> counter);
ScanPlan auto_spill(const ScanPlan& plan);
ScanPlan select(const ScanPlan& plan, dstd::Vector<dstd::String> cols);
ScanPlan name_columns(const ScanPlan& plan,
                      dstd::Vector<dataframe::Field> columns);
ScanPlan limit(const ScanPlan& plan, std::uint64_t n);
ScanPlan offset(const ScanPlan& plan, std::uint64_t n);
ScanPlan sort_by(const ScanPlan& plan, dstd::String column,
                 bool descending = false);
ScanPlan topk(const ScanPlan& plan, dstd::String column, std::int64_t k,
              bool largest = true);
ScanPlan materialize(const ScanPlan& plan, std::uint64_t checkpoint_size,
                     std::uint64_t part_size);
/// The plan read by a trace operation: records without a time are skipped.
ScanPlan timed(const ScanPlan& plan);
ScanPlan record_schema(const ScanPlan& plan, const dstd::String& id);
ScanPlan cancel_when(const ScanPlan& plan, dstd::Function<bool()> pred);
ScanPlan ordered(const ScanPlan& plan);
ScanPlan rollup_root(const ScanPlan& plan, dstd::String dir);
ScanPlan views_root(const ScanPlan& plan, dstd::String dir);
coro::CoroTask<dstd::Result<ExportStats>> for_each_batch(
    ScanPlan plan,
    dstd::Function<void(std::size_t, const dstd::Vector<dstd::StringView>&)>
        on_batch,
    std::size_t slot_count, std::uint64_t limit = 0);
dstd::Result<bool> export_would_bootstrap(const ScanPlan& plan);
dstd::Result<bool> collect_would_bootstrap(const ScanPlan& plan);
coro::CoroTask<dstd::Result<ExportStats>> export_json(ScanPlan plan,
                                                      ExportSink& sink);
coro::CoroTask<dstd::Result<ExportStats>> export_trace(ScanPlan plan,
                                                       TraceWriteOptions opts);
dataframe::LazyFrame collect(const ScanPlan& plan);
coro::AsyncGenerator<dstd::Result<dataframe::DataFrame>> stream(
    ScanPlan plan, std::int64_t batch_size = 0);
coro::CoroTask<dstd::Result<dataframe::DataFrame>> collect_frame(ScanPlan plan);
bool is_row_query(const ScanPlan& plan);
coro::CoroTask<dstd::Result<dataframe::DataFrame>> call_tree(
    ScanPlan plan, dstd::Vector<dstd::String> partition, dstd::String ts,
    dstd::String dur, dstd::String name);
coro::CoroTask<dstd::Result<dataframe::DataFrame>> flamegraph(
    ScanPlan plan, dstd::Vector<dstd::String> partition, dstd::String ts,
    dstd::String dur, dstd::String name, dstd::Vector<dstd::String> group);
coro::CoroTask<
    dstd::Result<dstd::Pair<dataframe::DataFrame, dataframe::DataFrame>>>
containment(ScanPlan plan, dstd::Vector<dstd::String> partition,
            dstd::String ts, dstd::String dur, dstd::String name,
            dstd::Vector<dstd::String> group);
coro::CoroTask<dstd::Result<dstd::String>> flamegraph_partial(
    ScanPlan plan, dstd::Vector<dstd::String> partition, dstd::String ts,
    dstd::String dur, dstd::String name, dstd::Vector<dstd::String> group);
dataframe::DataFrame merge_flamegraph_partials(
    const dstd::Vector<dstd::StringView>& partials);
coro::CoroTask<dstd::Result<ExportStats>> run_folds(
    ScanPlan plan, dstd::Span<detail::Fold* const> folds,
    dftracer::utils::dstd::StringIntern& intern,
    detail::DynamicPrune* dyn_prune = nullptr,
    coro::AsyncSemaphore* gate = nullptr);
coro::CoroTask<dstd::Result<ExportStats>> run(
    ScanPlan plan, const ProgressFn* progress = nullptr);
dstd::Result<dstd::String> materialize_dir(const ScanPlan& plan);
dstd::Result<void> register_materialized(const ScanPlan& plan,
                                         const dstd::String& dir);
dstd::Result<dstd::Vector<dstd::String>> mv_source(const ScanPlan& plan);
coro::CoroTask<dstd::Result<TypedResult>> collect_typed(
    ScanPlan plan, int shard_begin, int shard_end,
    const ProgressFn* progress = nullptr);
coro::CoroTask<dstd::Result<ExportStats>> export_counters(ScanPlan plan,
                                                          ExportSink& sink);
coro::CoroTask<dstd::Result<void>> materialize_partials(
    ScanPlan plan, const dstd::Vector<dstd::StringView>& partials);
dstd::Result<dstd::Optional<dataframe::DataFrame>> reconstruct_if_cached(
    const ScanPlan& plan);
coro::CoroTask<dstd::Result<dstd::String>> aggregate_partial(ScanPlan plan);
dstd::Result<dataframe::DataFrame> merge_partials_to_table(
    const ScanPlan& plan, const dstd::Vector<dstd::StringView>& partials);
dstd::Result<ExportStats> merge_counter_partials(
    const ScanPlan& plan, const dstd::Vector<dstd::StringView>& partials,
    ExportSink& sink);

}  // namespace dftracer::utils::trace::views::detail::scan

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLAN_OPS_H
