#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLAN_OPS_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLAN_OPS_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The trace scan as functions on an immutable ViewPlan: builders return a new
// plan with one setting changed, terminals execute a plan. View, ViewSource,
// ViewSession and the C ABI build and run their plans through these. A
// terminal takes its plan by value, so the task owns it.
namespace dftracer::utils::trace::views::detail::scan {

using detail::ScanPlan;

ScanPlan from_files(std::vector<ViewFile> files,
                    indexing::BloomFilterCache* bloom_cache = nullptr);
ScanPlan from_file(std::string file_path, std::string index_path = "");
coro::CoroTask<ScanPlan> from_directory(std::string dir,
                                        std::string index_path = "");
std::vector<TraceConfig> config(const ScanPlan& plan_);
std::vector<std::string> columns(const ScanPlan& plan_);
std::vector<ColumnInfo> schema(const ScanPlan& plan_);
std::unordered_map<std::string, dataframe::TypeId> column_types(
    const ScanPlan& plan_);
TimeMetric time_metric(const ScanPlan& plan_);
ScanPlan filter(const ScanPlan& plan_, Query q);
ScanPlan query(const ScanPlan& plan_, const std::string& dsl);
ScanPlan phase(const ScanPlan& plan_, Phase p);
ScanPlan time_range(const ScanPlan& plan_, double begin, double end);
ScanPlan time_bucket(const ScanPlan& plan_, std::uint64_t interval_us);
ScanPlan time_bucket(const ScanPlan& plan_, std::uint64_t interval_us,
                     std::uint64_t origin_us);
ScanPlan time_bucket_min(const ScanPlan& plan_, std::uint64_t interval_us);
ScanPlan occ_cell(const ScanPlan& plan_, std::uint64_t cell_us);
ScanPlan time_scale(const ScanPlan& plan_, double ns_ratio);
ScanPlan group_by(const ScanPlan& plan_, std::vector<GroupKey> keys);
ScanPlan agg(const ScanPlan& plan_, std::vector<AggSpec> specs);
ScanPlan agg(const ScanPlan& plan_, std::vector<FieldAggExpr> exprs);
ScanPlan agg_numeric_args(const ScanPlan& plan_);
ScanPlan agg_numeric_args(const ScanPlan& plan_,
                          std::vector<AggSpec> reductions);
ScanPlan memory_budget(const ScanPlan& plan_, std::uint64_t bytes);
ScanPlan auto_spill(const ScanPlan& plan_);
ScanPlan select(const ScanPlan& plan_, std::vector<std::string> cols);
ScanPlan limit(const ScanPlan& plan_, std::uint64_t n);
ScanPlan offset(const ScanPlan& plan_, std::uint64_t n);
ScanPlan sort_by(const ScanPlan& plan_, std::string column,
                 bool descending = false);
ScanPlan topk(const ScanPlan& plan_, std::string column, std::int64_t k,
              bool largest = true);
ScanPlan materialize(const ScanPlan& plan_, std::uint64_t checkpoint_size,
                     std::uint64_t part_size);
ScanPlan metadata(const ScanPlan& plan_, bool include);
ScanPlan cancel_when(const ScanPlan& plan_, std::function<bool()> pred);
ScanPlan rollup_root(const ScanPlan& plan_, std::string dir);
ScanPlan views_root(const ScanPlan& plan_, std::string dir);
ScanPlan emit_all_metadata(const ScanPlan& plan_, bool v);
coro::CoroTask<ExportStats> for_each_batch(
    ScanPlan plan_,
    std::function<void(std::size_t, const std::vector<std::string_view>&)>
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
    ScanPlan plan_, std::vector<std::string> partition, std::string ts,
    std::string dur, std::string name);
coro::CoroTask<dataframe::DataFrame> flamegraph(
    ScanPlan plan_, std::vector<std::string> partition, std::string ts,
    std::string dur, std::string name, std::vector<std::string> group);
coro::CoroTask<std::pair<dataframe::DataFrame, dataframe::DataFrame>>
containment(ScanPlan plan_, std::vector<std::string> partition, std::string ts,
            std::string dur, std::string name, std::vector<std::string> group);
coro::CoroTask<std::string> flamegraph_partial(
    ScanPlan plan_, std::vector<std::string> partition, std::string ts,
    std::string dur, std::string name, std::vector<std::string> group);
dataframe::DataFrame merge_flamegraph_partials(
    const std::vector<std::string_view>& partials);
coro::CoroTask<ExportStats> run_folds(
    ScanPlan plan_, std::span<detail::Fold* const> folds,
    dftracer::utils::StringIntern& intern,
    detail::DynamicPrune* dyn_prune = nullptr);
coro::CoroTask<ExportStats> run(ScanPlan plan_,
                                const ProgressFn* progress = nullptr);
std::string materialize_dir(const ScanPlan& plan_);
void register_materialized(const ScanPlan& plan_, const std::string& dir);
std::vector<std::string> mv_source(const ScanPlan& plan_);
coro::CoroTask<TypedResult> collect_typed(ScanPlan plan_, int shard_begin,
                                          int shard_end,
                                          const ProgressFn* progress = nullptr);
coro::CoroTask<ExportStats> export_counters(ScanPlan plan_, ExportSink& sink);
coro::CoroTask<void> materialize_partials(
    ScanPlan plan_, const std::vector<std::string_view>& partials);
std::optional<dataframe::DataFrame> reconstruct_if_cached(
    const ScanPlan& plan_);
coro::CoroTask<std::string> aggregate_partial(ScanPlan plan_);
dataframe::DataFrame merge_partials_to_table(
    const ScanPlan& plan_, const std::vector<std::string_view>& partials);
ExportStats merge_counter_partials(
    const ScanPlan& plan_, const std::vector<std::string_view>& partials,
    ExportSink& sink);

}  // namespace dftracer::utils::trace::views::detail::scan

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLAN_OPS_H
