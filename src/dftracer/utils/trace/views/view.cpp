#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/common/string_arena.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/json/parser.h>
#include <dftracer/utils/trace/comparator/compare_view.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/time_metric.h>
#include <dftracer/utils/trace/views/containment_fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/mv_store.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/trace/views/view_source.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace dftracer::utils::trace::views {

namespace {
// A mutable copy of a plan (or a fresh one), ready for one op to mutate.
std::shared_ptr<detail::ViewPlan> clone(
    const std::shared_ptr<const detail::ViewPlan>& base) {
    return base ? std::make_shared<detail::ViewPlan>(*base)
                : std::make_shared<detail::ViewPlan>();
}

// time_bucket() adds the bucket as an implicit leading group key, so listing
// "time_bucket" in group_by would double it (and the extra key resolves to a
// missing event field - an empty column). Drop the redundant key.
void strip_redundant_time_bucket(detail::ViewPlan& p) {
    if (p.time_bucket_us == 0) return;
    auto& g = p.group_by;
    g.erase(std::remove_if(g.begin(), g.end(),
                           [](const GroupKey& k) {
                               return k.kind == GroupKey::Kind::Field &&
                                      k.arg == "time_bucket";
                           }),
            g.end());
}
}  // namespace

View::View() : plan_(std::make_shared<detail::ViewPlan>()) {}

View::View(std::shared_ptr<const detail::ViewPlan> plan)
    : plan_(std::move(plan)) {}

View View::from_files(std::vector<ViewFile> files,
                      indexing::BloomFilterCache* bloom_cache) {
    auto plan = std::make_shared<detail::ViewPlan>();
    plan->files = std::move(files);
    plan->bloom_cache = bloom_cache;
    return View(std::move(plan));
}

View View::from_file(std::string file_path, std::string index_path) {
    std::vector<ViewFile> files;
    files.push_back(ViewFile{std::move(file_path), std::move(index_path)});
    return from_files(std::move(files));
}

coro::CoroTask<View> View::from_directory(std::string dir,
                                          std::string index_path) {
    utilities::filesystem::PatternDirectoryScannerUtility scanner;
    utilities::filesystem::PatternDirectoryScannerUtilityInput input(
        std::move(dir), {".pfw.gz"}, /*recursive=*/true,
        /*populate_size=*/false);

    std::vector<utilities::filesystem::FileEntry> entries;
    co_await run_coro_scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        entries = co_await scanner(scope, input);
    });

    std::vector<ViewFile> files;
    files.reserve(entries.size());
    for (auto& e : entries) {
        std::string fp = e.path.string();
        files.push_back(
            ViewFile{fp, internal::determine_index_path(fp, index_path)});
    }
    std::sort(files.begin(), files.end(),
              [](const ViewFile& a, const ViewFile& b) {
                  return a.file_path < b.file_path;
              });
    co_return from_files(std::move(files));
}

std::vector<TraceConfig> View::config() const {
    std::vector<TraceConfig> out;
    for (const auto& f : plan_->files) {
        auto tail = read_trace_config(f.file_path);
        if (!tail.empty()) {
            for (auto& c : tail) out.push_back(std::move(c));
            continue;
        }
        if (f.index_path.empty()) continue;

        struct Sink : ExportSink {
            std::string buf;
            void write(std::string_view d) override { buf.append(d); }
        } sink;
        View::from_file(f.file_path, f.index_path)
            .query(R"(name == "end")")
            .metadata(false)
            .export_json(sink)
            .get();
        std::size_t pos = 0;
        while (pos < sink.buf.size()) {
            std::size_t nl = sink.buf.find('\n', pos);
            if (nl == std::string::npos) nl = sink.buf.size();
            if (auto c = parse_end_event(
                    std::string_view(sink.buf).substr(pos, nl - pos)))
                out.push_back(std::move(*c));
            pos = nl + 1;
        }
    }
    return out;
}

namespace {

namespace idx = dftracer::utils::utilities::indexer;

using ColTypeMap = dftracer::utils::StringViewMap<idx::ColumnType>;

void fold_col_map(ColTypeMap& into, const ColTypeMap& from) {
    for (const auto& [name, t] : from) {
        auto [pos, inserted] = into.emplace(name, t);
        if (!inserted) pos->second = idx::merge_column_type(pos->second, t);
    }
}

// Union the harvested column types across the view's distinct index roots,
// reading each index's metadata in parallel. Seeds the base axis fields and
// appends resolved.* aliases for present hash columns.
ColTypeMap harvest_column_types(const std::vector<ViewFile>& files) {
    // Distinct index roots (many files often share one index).
    std::vector<std::string> roots;
    for (const auto& f : files) {
        if (f.index_path.empty()) continue;
        if (std::find(roots.begin(), roots.end(), f.index_path) == roots.end())
            roots.push_back(f.index_path);
    }

    ColTypeMap merged;
    // Base axis fields are always present and not harvested as columns.
    merged.emplace("pid", idx::ColumnType::Int64);
    merged.emplace("tid", idx::ColumnType::Int64);
    merged.emplace("ts", idx::ColumnType::Int64);
    merged.emplace("dur", idx::ColumnType::Int64);

    if (!roots.empty()) {
        const auto n = static_cast<std::int64_t>(roots.size());
        ColTypeMap harvested = default_runtime().parallel_reduce<ColTypeMap>(
            n, 1, ColTypeMap{},
            [&](std::int64_t begin, std::int64_t end) {
                ColTypeMap local;
                for (std::int64_t i = begin; i < end; ++i) {
                    try {
                        idx::IndexDatabase db(
                            roots[static_cast<std::size_t>(i)],
                            idx::IndexOpenMode::ReadOnly);
                        for (auto& [name, t] : db.query_all_column_types())
                            local.emplace(std::move(name), t);
                    } catch (...) {
                        // A missing or unreadable index contributes nothing.
                    }
                }
                return local;
            },
            [](ColTypeMap a, ColTypeMap b) {
                fold_col_map(a, b);
                return a;
            });
        fold_col_map(merged, harvested);
    }

    // resolved.* virtual columns, present when their hash column is.
    if (merged.count("fhash"))
        merged.emplace("resolved.fpath", idx::ColumnType::String);
    if (merged.count("hhash"))
        merged.emplace("resolved.hostname", idx::ColumnType::String);
    return merged;
}

}  // namespace

std::vector<std::string> View::columns() const {
    ColTypeMap m = harvest_column_types(plan_->files);
    std::vector<std::string> out;
    out.reserve(m.size());
    for (auto& [name, t] : m) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<View::ColumnInfo> View::schema() const {
    ColTypeMap m = harvest_column_types(plan_->files);
    std::vector<ColumnInfo> out;
    out.reserve(m.size());
    for (auto& [name, t] : m) {
        // A pre-v12 index stored no type; report it as a string.
        const char* tn = idx::column_type_name(t);
        out.push_back(ColumnInfo{name, tn[0] ? tn : "string"});
    }
    std::sort(out.begin(), out.end(),
              [](const ColumnInfo& a, const ColumnInfo& b) {
                  return a.name < b.name;
              });
    return out;
}

std::unordered_map<std::string, dataframe::TypeId> View::column_types() const {
    namespace df = dftracer::utils::dataframe;
    ColTypeMap m = harvest_column_types(plan_->files);
    std::unordered_map<std::string, df::TypeId> out;
    out.reserve(m.size());
    for (auto& [name, t] : m) {
        df::TypeId id = df::TypeId::Unknown;
        switch (t) {
            case idx::ColumnType::Int64:
                id = df::TypeId::Int64;
                break;
            case idx::ColumnType::Float64:
                id = df::TypeId::Float64;
                break;
            case idx::ColumnType::String:
                id = df::TypeId::String;
                break;
            case idx::ColumnType::Unknown:
                id = df::TypeId::Unknown;
                break;
        }
        out.emplace(name, id);
    }
    return out;
}

TimeMetric View::time_metric() const {
    if (plan_->files.empty()) return TimeMetric::US;
    return trace::read_time_metric(plan_->files.front().file_path);
}

View View::filter(Query q) const {
    auto next = clone(plan_);
    if (next->query) {
        // AND the two predicates by combining their source strings.
        std::string combined =
            "(" + next->query->source() + ") and (" + q.source() + ")";
        next->query = query::parse_or_throw(combined);
    } else {
        next->query = std::move(q);
    }
    return View(std::move(next));
}

View View::query(const std::string& dsl) const {
    return filter(query::parse_or_throw(dsl));
}

View View::phase(Phase p) const {
    auto next = clone(plan_);
    next->phase = p;
    return View(std::move(next));
}

View View::time_range(double begin, double end) const {
    auto next = clone(plan_);
    next->time_range = std::make_pair(begin, end);
    return View(std::move(next));
}

View View::time_bucket(std::uint64_t interval_us) const {
    auto next = clone(plan_);
    next->time_bucket_us = interval_us;
    strip_redundant_time_bucket(*next);
    return View(std::move(next));
}

View View::time_bucket(std::uint64_t interval_us,
                       std::uint64_t origin_us) const {
    auto next = clone(plan_);
    next->time_bucket_us = interval_us;
    next->bucket_origin_us = origin_us;
    next->bucket_origin_min = false;
    strip_redundant_time_bucket(*next);
    return View(std::move(next));
}

View View::time_bucket_min(std::uint64_t interval_us) const {
    auto next = clone(plan_);
    next->time_bucket_us = interval_us;
    next->bucket_origin_min = true;
    strip_redundant_time_bucket(*next);
    return View(std::move(next));
}

View View::occ_cell(std::uint64_t cell_us) const {
    auto next = clone(plan_);
    next->occ_cell_us = cell_us;
    return View(std::move(next));
}

View View::time_scale(double ns_ratio) const {
    auto next = clone(plan_);
    next->time_scale = ns_ratio;
    return View(std::move(next));
}

AggregatedView View::group_by(std::vector<GroupKey> keys) const {
    auto next = clone(plan_);
    next->group_by = std::move(keys);
    strip_redundant_time_bucket(*next);
    return AggregatedView(View(std::move(next)));
}

AggregatedView View::agg(std::vector<AggSpec> specs) const {
    auto next = clone(plan_);
    next->agg = std::move(specs);
    return AggregatedView(View(std::move(next)));
}

namespace {

// Lower a named (non-wildcard) field aggregate to an AggSpec. The field is
// carried through verbatim, so dotted args.* names resolve exactly like the
// string / AggSpec forms.
AggSpec lower_field_agg(const FieldAggExpr& e) {
    using dftracer::utils::dataframe::field::AggFn;
    switch (e.fn()) {
        case AggFn::Count:
            return AggSpec(AggOp::Count);
        case AggFn::Sum:
            return AggSpec(AggOp::Sum, e.field());
        case AggFn::Min:
            return AggSpec(AggOp::Min, e.field());
        case AggFn::Max:
            return AggSpec(AggOp::Max, e.field());
        case AggFn::Mean:
            return AggSpec(AggOp::Mean, e.field());
        case AggFn::Var:
            return AggSpec(AggOp::Var, e.field());
        case AggFn::Std:
            return AggSpec(AggOp::Std, e.field());
        case AggFn::Skew:
            return AggSpec(AggOp::Skew, e.field());
        case AggFn::Kurt:
            return AggSpec(AggOp::Kurt, e.field());
        case AggFn::ArgMax:
            return AggSpec(AggOp::ArgMax, e.field(), "", e.by());
    }
    throw DFTUtilsException::cat(ErrorCode::INVALID_ARGUMENT,
                                 "unhandled aggregate function");
}

}  // namespace

AggregatedView View::agg(std::vector<FieldAggExpr> exprs) const {
    using dftracer::utils::dataframe::field::AggFn;
    std::vector<AggSpec> specs;
    specs.reserve(exprs.size());
    // Wildcard (F.any) reductions run over every discovered numeric arg. Mean
    // alone keeps the legacy bare column; other reductions (and mean alongside
    // them) emit `<op>_<arg>` columns via numeric_arg_aggs.
    std::vector<AggSpec> num_args;
    bool wildcard_mean = false;
    for (const auto& e : exprs) {
        if (e.wildcard()) {
            switch (e.fn()) {
                case AggFn::Mean:
                    wildcard_mean = true;
                    break;
                case AggFn::Count:
                    specs.push_back(AggSpec(AggOp::Count));
                    break;
                case AggFn::Sum:
                    num_args.push_back(AggSpec(AggOp::Sum));
                    break;
                case AggFn::Min:
                    num_args.push_back(AggSpec(AggOp::Min));
                    break;
                case AggFn::Max:
                    num_args.push_back(AggSpec(AggOp::Max));
                    break;
                case AggFn::Var:
                    num_args.push_back(AggSpec(AggOp::Var));
                    break;
                case AggFn::Std:
                    num_args.push_back(AggSpec(AggOp::Std));
                    break;
                case AggFn::Skew:
                    num_args.push_back(AggSpec(AggOp::Skew));
                    break;
                case AggFn::Kurt:
                    num_args.push_back(AggSpec(AggOp::Kurt));
                    break;
                default:
                    throw DFTUtilsException::cat(
                        ErrorCode::INVALID_ARGUMENT,
                        "F.any supports .count()/.mean()/.sum()/.min()/.max()/"
                        ".var()/.std()/.skew()/.kurt(); name a field for "
                        "argmax "
                        "or percentile, e.g. F(\"args.level\").pct(0.9)");
            }
            continue;
        }
        specs.push_back(lower_field_agg(e));
    }
    View v = *this;
    if (!specs.empty()) v = v.agg(std::move(specs));
    if (wildcard_mean && num_args.empty()) {
        v = v.agg_numeric_args();
    } else {
        if (wildcard_mean) num_args.push_back(AggSpec(AggOp::Mean));
        if (!num_args.empty()) v = v.agg_numeric_args(std::move(num_args));
    }
    return AggregatedView(std::move(v));
}

AggregatedView View::agg_numeric_args() const {
    auto next = clone(plan_);
    next->auto_numeric_metrics = true;
    return AggregatedView(View(std::move(next)));
}

AggregatedView View::agg_numeric_args(std::vector<AggSpec> reductions) const {
    for (const auto& s : reductions)
        if (!is_dyn_reduction(s.op))
            throw DFTUtilsException::cat(
                ErrorCode::INVALID_ARGUMENT,
                "agg_numeric_args supports only Count/Sum/Min/Max/SumSq/Mean/"
                "Var/Std/Skew/Kurt over numeric args; Pct/Hist/ArgMax need a "
                "named field");
    auto next = clone(plan_);
    next->auto_numeric_metrics = true;
    next->numeric_arg_aggs = std::move(reductions);
    return AggregatedView(View(std::move(next)));
}

View View::memory_budget(std::uint64_t bytes) const {
    auto next = clone(plan_);
    next->memory_budget = bytes;
    return View(std::move(next));
}

View View::auto_spill() const {
    // ~1/3 of available memory, floored so tiny machines still get a workable
    // in-core window before spilling.
    std::uint64_t budget =
        static_cast<std::uint64_t>(detect_available_memory() / 3);
    if (budget < MIN_MEMORY_BUDGET_BYTES) budget = MIN_MEMORY_BUDGET_BYTES;
    return memory_budget(budget);
}

View View::select(std::vector<std::string> cols) const {
    auto next = clone(plan_);
    next->select = std::move(cols);
    return View(std::move(next));
}

View View::limit(std::uint64_t n) const {
    auto next = clone(plan_);
    next->limit = n;
    return View(std::move(next));
}

View View::offset(std::uint64_t n) const {
    auto next = clone(plan_);
    next->offset = n;
    return View(std::move(next));
}

View View::sort_by(std::string column, bool descending) const {
    auto next = clone(plan_);
    next->sort_col = std::move(column);
    next->sort_desc = descending;
    return View(std::move(next));
}

View View::topk(std::string column, std::int64_t k, bool largest) const {
    auto next = clone(plan_);
    next->topk_col = std::move(column);
    next->topk_k = k;
    next->topk_largest = largest;
    return View(std::move(next));
}

View View::materialize(std::uint64_t checkpoint_size,
                       std::uint64_t part_size) const {
    auto next = clone(plan_);
    next->materialize = true;
    next->mv_checkpoint_size = checkpoint_size;
    next->mv_part_size = part_size;
    return View(std::move(next));
}

View View::metadata(bool include) const {
    auto next = clone(plan_);
    next->include_metadata = include;
    return View(std::move(next));
}

View View::cancel_when(std::function<bool()> pred) const {
    auto next = clone(plan_);
    next->cancelled = std::move(pred);
    return View(std::move(next));
}

View View::rollup_root(std::string dir) const {
    auto next = clone(plan_);
    next->rollup_root = std::move(dir);
    return View(std::move(next));
}

View View::views_root(std::string dir) const {
    auto next = clone(plan_);
    next->views_root = std::move(dir);
    return View(std::move(next));
}

View View::emit_all_metadata(bool v) const {
    auto next = clone(plan_);
    next->emit_all_metadata = v;
    return View(std::move(next));
}

coro::CoroTask<ExportStats> View::for_each_batch(
    std::function<void(std::size_t, const std::vector<std::string_view>&)>
        on_batch,
    std::size_t num_slots, std::uint64_t limit) const {
    co_return co_await detail::run_scan_batches(plan_, num_slots, limit,
                                                on_batch);
}

bool View::export_would_bootstrap() const {
    return detail::export_bootstrap_eligible(*plan_);
}

bool View::collect_would_bootstrap() const {
    return detail::collect_bootstrap_eligible(*plan_);
}

coro::CoroTask<ExportStats> View::export_json(ExportSink& sink) const {
    co_return co_await detail::run_export(*plan_, sink);
}

coro::CoroTask<ExportStats> View::export_trace(TraceWriteOptions opts) const {
    co_return co_await detail::run_export_trace(*plan_, opts);
}

dataframe::LazyFrame View::collect() const {
    // A row-query select is built by the scan producer itself (NativeRowFold /
    // StreamRowFold, via build_row_frame's select branch), which emits exactly
    // the selected columns. Keep it in the scan plan rather than stripping it
    // and re-projecting with LazyFrame::select: a streaming morsel's schema is
    // data-dependent (its arg columns are discovered at scan time), so a
    // LazyFrame projection resolved against the source's index-derived schema
    // would miss an un-indexed arg column and index the morsel out of bounds.
    const bool row_query = detail::is_row_query(*plan_);
    const bool select_in_scan = row_query && !plan_->select.empty();

    std::shared_ptr<detail::ViewPlan> stripped = clone(plan_);
    if (!select_in_scan) stripped->select.clear();
    stripped->sort_col.clear();
    stripped->sort_desc = false;
    stripped->topk_col.clear();
    stripped->topk_k = -1;
    stripped->topk_largest = true;
    stripped->offset = 0;
    stripped->limit = 0;

    dataframe::LazyFrame lf =
        dataframe::LazyFrame::scan(
            std::make_shared<ViewSource>(View(std::move(stripped))))
            .memory_budget(plan_->memory_budget);

    // A row query's scan producer canonicalizes arg column names to
    // "args.<key>" (see build_row_frame); sort/topk on a bare arg name must
    // resolve to that same column.
    if (!plan_->sort_col.empty())
        lf = lf.sort_by(row_query
                            ? detail::canonical_row_column_name(plan_->sort_col)
                            : plan_->sort_col,
                        plan_->sort_desc);
    if (!plan_->topk_col.empty())
        lf = lf.topk(row_query
                         ? detail::canonical_row_column_name(plan_->topk_col)
                         : plan_->topk_col,
                     plan_->topk_k, plan_->topk_largest);
    if (plan_->offset || plan_->limit) {
        const std::int64_t off = static_cast<std::int64_t>(plan_->offset);
        const std::int64_t len = plan_->limit
                                     ? static_cast<std::int64_t>(plan_->limit)
                                     : std::numeric_limits<std::int64_t>::max();
        lf = lf.slice(off, len);
    }
    if (!select_in_scan && !plan_->select.empty())
        lf = lf.select(plan_->select);
    return lf;
}

coro::AsyncGenerator<dataframe::DataFrame> View::stream(
    std::int64_t morsel_rows) const {
    dataframe::LazyFrame lf = collect();
    auto gen = lf.stream(morsel_rows);
    while (auto df = co_await gen.next()) co_yield std::move(*df);
}

coro::CoroTask<dataframe::DataFrame> View::collect_frame() const {
    // A row query (no group_by/agg) returns the matching events, not a count.
    if (detail::is_row_query(*plan_))
        co_return co_await detail::run_collect_rows(*plan_);
    // Every aggregation runs through the dataframe engine; materialize() opts
    // in to persisting its AggState partials as the rollup in the same pass.
    co_return detail::apply_agg_post_ops(
        co_await detail::run_collect_via_engine(*plan_), *plan_);
}

bool View::is_row_query() const { return detail::is_row_query(*plan_); }

coro::CoroTask<dataframe::DataFrame> View::call_tree(
    std::vector<std::string> partition, std::string ts, std::string dur,
    std::string name) const {
    co_return co_await detail::run_call_tree(*plan_, std::move(partition),
                                             std::move(ts), std::move(dur),
                                             std::move(name));
}

coro::CoroTask<dataframe::DataFrame> View::flamegraph(
    std::vector<std::string> partition, std::string ts, std::string dur,
    std::string name, std::vector<std::string> group) const {
    co_return co_await detail::run_flamegraph(
        *plan_, std::move(partition), std::move(ts), std::move(dur),
        std::move(name), std::move(group));
}

coro::CoroTask<std::pair<dataframe::DataFrame, dataframe::DataFrame> >
View::containment(std::vector<std::string> partition, std::string ts,
                  std::string dur, std::string name,
                  std::vector<std::string> group) const {
    co_return co_await detail::run_containment(
        *plan_, std::move(partition), std::move(ts), std::move(dur),
        std::move(name), std::move(group));
}

coro::CoroTask<std::string> View::flamegraph_partial(
    std::vector<std::string> partition, std::string ts, std::string dur,
    std::string name, std::vector<std::string> group) const {
    co_return co_await detail::run_flamegraph_partial(
        *plan_, std::move(partition), std::move(ts), std::move(dur),
        std::move(name), std::move(group));
}

dataframe::DataFrame View::merge_flamegraph_partials(
    const std::vector<std::string_view>& partials) {
    return detail::merge_flamegraph_partials(partials);
}

coro::CoroTask<ExportStats> View::run_folds(
    std::span<detail::Fold* const> folds, dftracer::utils::StringIntern& intern,
    detail::DynamicPrune* dyn_prune) const {
    co_return co_await detail::run_folds(*plan_, folds, intern, dyn_prune);
}

coro::CoroTask<ExportStats> View::run(const ProgressFn* progress) const {
    co_return co_await detail::run_materialize(*plan_, progress);
}

std::string View::materialize_dir() const {
    return detail::materialize_view_dir(*plan_);
}

void View::register_materialized(const std::string& dir) const {
    detail::register_view(dir, *plan_);
}

std::vector<std::string> View::mv_source() const {
    std::vector<std::string> out;
    if (auto mv = detail::find_subsuming_view(*plan_))
        for (const auto& f : *mv) out.push_back(f.file_path);
    return out;
}

coro::CoroTask<TypedResult> View::collect_typed(
    int shard_begin, int shard_end, const ProgressFn* progress) const {
    co_return co_await detail::run_collect_typed(*plan_, shard_begin, shard_end,
                                                 progress);
}

coro::CoroTask<ExportStats> View::export_counters(ExportSink& sink) const {
    co_return co_await detail::run_export_counters(*plan_, sink);
}

coro::CoroTask<void> View::materialize_partials(
    const std::vector<std::string_view>& partials) const {
    co_return co_await detail::run_materialize_partials(*plan_, partials);
}

std::optional<dataframe::DataFrame> View::reconstruct_if_cached() const {
    return detail::run_reconstruct_if_cached(*plan_);
}

coro::CoroTask<std::string> View::aggregate_partial() const {
    co_return co_await detail::run_aggregate_partial(*plan_);
}

dataframe::DataFrame View::merge_partials_to_table(
    const std::vector<std::string_view>& partials) const {
    return detail::merge_partials_to_table(*plan_, partials);
}

ExportStats View::merge_counter_partials(
    const std::vector<std::string_view>& partials, ExportSink& sink) const {
    return detail::merge_counters_partials(*plan_, partials, sink);
}

ViewSession View::session() const { return ViewSession(plan_); }

ViewSession::ViewSession(std::shared_ptr<const detail::ViewPlan> plan)
    : state_(detail::make_view_session_state(std::move(plan))) {}

void ViewSession::attach_fold(
    Query predicate,
    std::function<
        std::function<void(const json::JsonValue&, std::string_view)>()>
        make_consumer,
    std::function<void()> finalize) {
    detail::add_fold_branch(*state_, std::move(predicate),
                            std::move(make_consumer), std::move(finalize));
}

void ViewSession::attach_fold_factory(
    std::function<std::unique_ptr<detail::Fold>(dftracer::utils::StringIntern&)>
        make,
    std::function<void()> finalize) {
    detail::add_fold_factory(*state_, std::move(make), std::move(finalize));
}

void ViewSession::propose_base_prune(Query q) {
    detail::propose_base_prune(*state_, std::move(q));
}

Deferred<dataframe::DataFrame> ViewSession::collect(
    Query predicate, std::vector<GroupKey> group_by, std::vector<AggSpec> agg) {
    auto out = std::make_shared<dataframe::DataFrame>();
    key_counts_.emplace_back(out.get(),
                             static_cast<std::int64_t>(group_by.size()));
    // A predicated collect runs as an engine agg branch that filters per event
    // (apply_query), overlaying the predicate onto the base plan on the shared
    // scan; no per-branch predicate, so it does not also join the raw driver.
    detail::BranchHooks h;
    h.agg = detail::AggBranch{std::move(group_by),
                              std::move(agg),
                              out,
                              nullptr,
                              /*apply_query=*/true,
                              nullptr,
                              std::move(predicate)};
    detail::add_branch(*state_, std::move(h));
    return {out, executed_};
}

Deferred<dataframe::DataFrame> ViewSession::collect(
    std::vector<GroupKey> group_by, std::vector<AggSpec> agg) {
    auto out = std::make_shared<dataframe::DataFrame>();
    key_counts_.emplace_back(out.get(),
                             static_cast<std::int64_t>(group_by.size()));
    // No predicate: match all scanned events. The descriptor lets execute()
    // serve this branch from a rollup or the tier instead of scanning.
    detail::BranchHooks h;
    h.agg = detail::AggBranch{
        std::move(group_by), std::move(agg), out, nullptr, false, nullptr,
        std::nullopt};
    detail::add_branch(*state_, std::move(h));
    return {out, executed_};
}

Deferred<dataframe::DataFrame> ViewSession::collect(const View& branch) {
    auto out = std::make_shared<dataframe::DataFrame>();
    const auto& bp = *branch.plan_;
    // Output key columns are [time_bucket?, group_by...], so a bucketed branch
    // has one more leading key column than its group_by (matches join layout).
    key_counts_.emplace_back(out.get(),
                             static_cast<std::int64_t>(bp.group_by.size()) +
                                 (bp.time_bucket_us > 0 ? 1 : 0));
    detail::BranchHooks h;
    h.agg = detail::AggBranch{
        bp.group_by,          bp.agg,  out,         branch.plan_,
        bp.query.has_value(), nullptr, std::nullopt};
    detail::add_branch(*state_, std::move(h));
    return {out, executed_};
}

std::int64_t ViewSession::key_count_of(const void* out) const {
    for (const auto& [ptr, n] : key_counts_)
        if (ptr == out) return n;
    return -1;
}

Deferred<dataframe::DataFrame> ViewSession::join(
    Deferred<dataframe::DataFrame> left, Deferred<dataframe::DataFrame> right,
    JoinType how, std::int64_t n_key) {
    if (!left.value_ || !right.value_ || left.executed_ != executed_ ||
        right.executed_ != executed_)
        throw DFTUtilsException::cat(
            ErrorCode::INVALID_ARGUMENT,
            "ViewSession::join: both handles must be collect() results of "
            "this session");
    if (n_key < 0) n_key = key_count_of(left.value_.get());
    if (n_key < 0)
        throw DFTUtilsException::cat(
            ErrorCode::INVALID_ARGUMENT,
            "ViewSession::join: cannot infer n_key; pass it explicitly");
    auto out = std::make_shared<dataframe::DataFrame>();
    combines_.emplace_back(
        [out, l = left.value_, r = right.value_, n_key, how]() {
            *out = join_batches(*l, *r, n_key, how);
        });
    return {out, executed_};
}

Deferred<dataframe::DataFrame> ViewSession::compare(
    Deferred<dataframe::DataFrame> baseline,
    Deferred<dataframe::DataFrame> variant, std::int64_t n_key) {
    if (!baseline.value_ || !variant.value_ ||
        baseline.executed_ != executed_ || variant.executed_ != executed_)
        throw DFTUtilsException::cat(
            ErrorCode::INVALID_ARGUMENT,
            "ViewSession::compare: both handles must be collect() results of "
            "this session");
    if (n_key < 0) n_key = key_count_of(baseline.value_.get());
    if (n_key < 0)
        throw DFTUtilsException::cat(
            ErrorCode::INVALID_ARGUMENT,
            "ViewSession::compare: cannot infer n_key; pass it explicitly");
    auto out = std::make_shared<dataframe::DataFrame>();
    combines_.emplace_back(
        [out, b = baseline.value_, v = variant.value_, n_key]() {
            *out = comparator::CompareView::compare_batches(*b, *v, n_key);
        });
    return {out, executed_};
}

Deferred<std::string> ViewSession::aggregate_partial(const View& branch) {
    auto out =
        std::make_shared<dataframe::DataFrame>();  // unused; partial path
    auto partial = std::make_shared<std::string>();
    const auto& bp = *branch.plan_;
    detail::BranchHooks h;
    h.agg = detail::AggBranch{
        bp.group_by,          bp.agg,  out,         branch.plan_,
        bp.query.has_value(), partial, std::nullopt};
    detail::add_branch(*state_, std::move(h));
    return {partial, executed_};
}

void ViewSession::materialize(std::vector<GroupKey> group_by,
                              std::vector<AggSpec> agg) {
    detail::add_materialize_branch(*state_, std::move(group_by),
                                   std::move(agg));
}

Deferred<ExportStats> ViewSession::export_json(Query predicate,
                                               ExportSink& sink) {
    auto out = std::make_shared<ExportStats>();
    detail::BranchHooks h = detail::make_export_branch(sink, out);
    h.predicate = std::move(predicate);
    detail::add_branch(*state_, std::move(h));
    return {out, executed_};
}

Deferred<ExportStats> ViewSession::export_json(ExportSink& sink) {
    auto out = std::make_shared<ExportStats>();
    detail::BranchHooks h = detail::make_export_branch(sink, out);
    detail::add_branch(*state_, std::move(h));
    return {out, executed_};
}

Deferred<dataframe::DataFrame> ViewSession::collect_events(const View& branch) {
    const auto& bp = *branch.plan_;
    auto out = std::make_shared<dataframe::DataFrame>();
    // Per-worker owned events built straight into native columns (no Arrow);
    // each worker interns its own strings, so build_row_frame resolves them per
    // worker.
    struct Slot {
        dftracer::utils::StringIntern intern;
        std::vector<detail::FoldEvent> events;
    };
    auto slots = std::make_shared<std::vector<std::shared_ptr<Slot> > >();
    auto select = std::make_shared<std::vector<std::string> >(bp.select);
    const double time_scale = bp.time_scale;

    auto make_consumer = [slots]() {
        auto slot = std::make_shared<Slot>();
        slots->push_back(slot);
        return [slot](const json::JsonValue& jv, std::string_view) {
            detail::FoldEvent fe = detail::extract_fold_event(
                jv.element(), slot->intern, /*needs_args=*/true);
            if (fe.phase == RecordPhase::METADATA ||
                fe.phase == RecordPhase::UNKNOWN)
                return;
            slot->events.push_back(std::move(fe));
        };
    };
    auto finalize = [slots, select, out, time_scale]() {
        std::vector<dataframe::DataFrame> frames;
        frames.reserve(slots->size());
        for (const auto& s : *slots) {
            if (s->events.empty()) continue;
            frames.push_back(detail::build_row_frame(s->events, s->intern,
                                                     *select, time_scale));
        }
        if (frames.empty()) return;  // out stays an empty frame
        std::vector<const dataframe::DataFrame*> parts;
        parts.reserve(frames.size());
        for (const auto& f : frames) parts.push_back(&f);
        // Slots discover different args, so union their schemas.
        *out = dataframe::concat(parts, dataframe::ConcatHow::Diagonal);
    };

    // effective_query folds the branch's phase into the predicate (bp.query
    // alone drops it), so a branch's phase() filters in a fused session.
    if (auto eq = detail::effective_query(bp))
        detail::add_fold_branch(*state_, std::move(*eq),
                                std::move(make_consumer), std::move(finalize));
    else
        detail::add_fold_branch(*state_, std::move(make_consumer),
                                std::move(finalize));
    return {out, executed_};
}

// One containment branch buffering rows once (shared intern for cross-slot lane
// consistency); finalize builds whichever of out_ct/out_fg is requested,
// sorting each lane once when both are.
void ViewSession::add_containment_branch(
    const View& branch, const std::vector<std::string>& partition,
    const std::string& ts, const std::string& dur, const std::string& name,
    std::shared_ptr<dataframe::DataFrame> out_ct,
    std::shared_ptr<dataframe::DataFrame> out_fg) {
    const auto& bp = *branch.plan_;
    auto intern = std::make_shared<dftracer::utils::StringIntern>();
    auto spec = std::make_shared<detail::ContainmentSpec>(
        detail::make_containment_spec(*intern, partition, ts, dur, name));
    auto bufs = std::make_shared<
        std::vector<std::shared_ptr<std::vector<detail::ContainmentRow> > > >();
    const double time_scale = bp.time_scale;

    // One shared intern across workers keeps lane ids consistent, so the
    // per-worker rows concatenate without a re-key.
    auto make_consumer = [intern, spec, bufs]() {
        auto rows = std::make_shared<std::vector<detail::ContainmentRow> >();
        bufs->push_back(rows);
        return
            [intern, spec, rows](const json::JsonValue& jv, std::string_view) {
                detail::FoldEvent fe = detail::extract_fold_event(
                    jv.element(), *intern, spec->needs_args,
                    &spec->nested_captures);
                detail::ContainmentRow r;
                if (detail::containment_row(fe, *spec, *intern, r))
                    rows->push_back(r);
            };
    };
    auto finalize = [intern, bufs, out_ct, out_fg, time_scale]() {
        std::vector<detail::ContainmentRow> all;
        for (const auto& rows : *bufs)
            all.insert(all.end(), rows->begin(), rows->end());
        if (out_ct && out_fg) {
            auto pr = detail::build_containment_both(all, *intern, time_scale);
            *out_ct = std::move(pr.first);
            *out_fg = std::move(pr.second);
        } else if (out_ct) {
            *out_ct = detail::build_call_tree(all, detail::sorted_lanes(all),
                                              *intern, time_scale);
        } else if (out_fg) {
            *out_fg = detail::build_flamegraph(all, detail::sorted_lanes(all),
                                               *intern, time_scale);
        }
    };

    if (bp.query)
        detail::add_fold_branch(*state_, *bp.query, std::move(make_consumer),
                                std::move(finalize));
    else
        detail::add_fold_branch(*state_, std::move(make_consumer),
                                std::move(finalize));
}

Deferred<dataframe::DataFrame> ViewSession::call_tree(
    const View& branch, std::vector<std::string> partition, std::string ts,
    std::string dur, std::string name) {
    auto out = std::make_shared<dataframe::DataFrame>();
    add_containment_branch(branch, partition, ts, dur, name, out, nullptr);
    return {out, executed_};
}

Deferred<dataframe::DataFrame> ViewSession::flamegraph(
    const View& branch, std::vector<std::string> partition, std::string ts,
    std::string dur, std::string name) {
    auto out = std::make_shared<dataframe::DataFrame>();
    add_containment_branch(branch, partition, ts, dur, name, nullptr, out);
    return {out, executed_};
}

ContainmentHandles ViewSession::containment(const View& branch,
                                            std::vector<std::string> partition,
                                            std::string ts, std::string dur,
                                            std::string name) {
    auto out_ct = std::make_shared<dataframe::DataFrame>();
    auto out_fg = std::make_shared<dataframe::DataFrame>();
    add_containment_branch(branch, partition, ts, dur, name, out_ct, out_fg);
    return {{out_ct, executed_}, {out_fg, executed_}};
}

coro::CoroTask<ExportStats> ViewSession::execute() {
    ExportStats stats = co_await detail::run_session(state_);
    // Branch outputs are populated; run the post-scan combines before flipping
    // executed_ so a combine reads the raw branch out, not a resolved handle.
    for (auto& c : combines_) c();
    *executed_ = true;
    co_return stats;
}

}  // namespace dftracer::utils::trace::views
