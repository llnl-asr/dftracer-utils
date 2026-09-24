#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/json/parser.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/time_metric.h>
#include <dftracer/utils/trace/views/containment_fold.h>
#include <dftracer/utils/trace/views/mv_store.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
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

namespace dftracer::utils::trace::views::detail::scan {

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

ScanPlan from_files(std::vector<ViewFile> files,
                    indexing::BloomFilterCache* bloom_cache) {
    auto plan = std::make_shared<detail::ViewPlan>();
    plan->files = std::move(files);
    plan->bloom_cache = bloom_cache;
    return plan;
}

ScanPlan from_file(std::string file_path, std::string index_path) {
    std::vector<ViewFile> files;
    files.push_back(ViewFile{std::move(file_path), std::move(index_path)});
    return from_files(std::move(files));
}

coro::CoroTask<ScanPlan> from_directory(std::string dir,
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

std::vector<TraceConfig> config(const ScanPlan& plan_) {
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
        export_json(metadata(query(from_file(f.file_path, f.index_path),
                                   R"(name == "end")"),
                             false),
                    sink)
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

std::vector<std::string> columns(const ScanPlan& plan_) {
    ColTypeMap m = harvest_column_types(plan_->files);
    std::vector<std::string> out;
    out.reserve(m.size());
    for (auto& [name, t] : m) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<ColumnInfo> schema(const ScanPlan& plan_) {
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

std::unordered_map<std::string, dataframe::TypeId> column_types(
    const ScanPlan& plan_) {
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

TimeMetric time_metric(const ScanPlan& plan_) {
    if (plan_->files.empty()) return TimeMetric::US;
    return trace::read_time_metric(plan_->files.front().file_path);
}

ScanPlan filter(const ScanPlan& plan_, Query q) {
    auto next = clone(plan_);
    if (next->query) {
        // AND the two predicates by combining their source strings.
        std::string combined =
            "(" + next->query->source() + ") and (" + q.source() + ")";
        next->query = query::parse_or_throw(combined);
    } else {
        next->query = std::move(q);
    }
    return next;
}

ScanPlan query(const ScanPlan& plan_, const std::string& dsl) {
    return filter(plan_, query::parse_or_throw(dsl));
}

ScanPlan phase(const ScanPlan& plan_, Phase p) {
    auto next = clone(plan_);
    next->phase = p;
    return next;
}

ScanPlan time_range(const ScanPlan& plan_, double begin, double end) {
    auto next = clone(plan_);
    next->time_range = std::make_pair(begin, end);
    return next;
}

ScanPlan time_bucket(const ScanPlan& plan_, std::uint64_t interval_us) {
    auto next = clone(plan_);
    next->time_bucket_us = interval_us;
    strip_redundant_time_bucket(*next);
    return next;
}

ScanPlan time_bucket(const ScanPlan& plan_, std::uint64_t interval_us,
                     std::uint64_t origin_us) {
    auto next = clone(plan_);
    next->time_bucket_us = interval_us;
    next->bucket_origin_us = origin_us;
    next->bucket_origin_min = false;
    strip_redundant_time_bucket(*next);
    return next;
}

ScanPlan time_bucket_min(const ScanPlan& plan_, std::uint64_t interval_us) {
    auto next = clone(plan_);
    next->time_bucket_us = interval_us;
    next->bucket_origin_min = true;
    strip_redundant_time_bucket(*next);
    return next;
}

ScanPlan occ_cell(const ScanPlan& plan_, std::uint64_t cell_us) {
    auto next = clone(plan_);
    next->occ_cell_us = cell_us;
    return next;
}

ScanPlan time_scale(const ScanPlan& plan_, double ns_ratio) {
    auto next = clone(plan_);
    next->time_scale = ns_ratio;
    return next;
}

ScanPlan group_by(const ScanPlan& plan_, std::vector<GroupKey> keys) {
    auto next = clone(plan_);
    next->group_by = std::move(keys);
    strip_redundant_time_bucket(*next);
    return next;
}

ScanPlan agg(const ScanPlan& plan_, std::vector<AggSpec> specs) {
    auto next = clone(plan_);
    next->agg = std::move(specs);
    return next;
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

ScanPlan agg(const ScanPlan& plan_, std::vector<FieldAggExpr> exprs) {
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
    ScanPlan v = plan_;
    if (!specs.empty()) v = agg(v, std::move(specs));
    if (wildcard_mean && num_args.empty()) {
        v = agg_numeric_args(v);
    } else {
        if (wildcard_mean) num_args.push_back(AggSpec(AggOp::Mean));
        if (!num_args.empty()) v = agg_numeric_args(v, std::move(num_args));
    }
    return v;
}

ScanPlan agg_numeric_args(const ScanPlan& plan_) {
    auto next = clone(plan_);
    next->auto_numeric_metrics = true;
    return next;
}

ScanPlan agg_numeric_args(const ScanPlan& plan_,
                          std::vector<AggSpec> reductions) {
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
    return next;
}

ScanPlan memory_budget(const ScanPlan& plan_, std::uint64_t bytes) {
    auto next = clone(plan_);
    next->memory_budget = bytes;
    return next;
}

ScanPlan auto_spill(const ScanPlan& plan_) {
    // ~1/3 of available memory, floored so tiny machines still get a workable
    // in-core window before spilling.
    std::uint64_t budget =
        static_cast<std::uint64_t>(detect_available_memory() / 3);
    if (budget < MIN_MEMORY_BUDGET_BYTES) budget = MIN_MEMORY_BUDGET_BYTES;
    return memory_budget(plan_, budget);
}

ScanPlan select(const ScanPlan& plan_, std::vector<std::string> cols) {
    auto next = clone(plan_);
    next->select = std::move(cols);
    return next;
}

ScanPlan limit(const ScanPlan& plan_, std::uint64_t n) {
    auto next = clone(plan_);
    next->limit = n;
    return next;
}

ScanPlan offset(const ScanPlan& plan_, std::uint64_t n) {
    auto next = clone(plan_);
    next->offset = n;
    return next;
}

ScanPlan sort_by(const ScanPlan& plan_, std::string column, bool descending) {
    auto next = clone(plan_);
    next->sort_col = std::move(column);
    next->sort_desc = descending;
    return next;
}

ScanPlan topk(const ScanPlan& plan_, std::string column, std::int64_t k,
              bool largest) {
    auto next = clone(plan_);
    next->topk_col = std::move(column);
    next->topk_k = k;
    next->topk_largest = largest;
    return next;
}

ScanPlan materialize(const ScanPlan& plan_, std::uint64_t checkpoint_size,
                     std::uint64_t part_size) {
    auto next = clone(plan_);
    next->materialize = true;
    next->mv_checkpoint_size = checkpoint_size;
    next->mv_part_size = part_size;
    return next;
}

ScanPlan metadata(const ScanPlan& plan_, bool include) {
    auto next = clone(plan_);
    next->include_metadata = include;
    return next;
}

ScanPlan cancel_when(const ScanPlan& plan_, std::function<bool()> pred) {
    auto next = clone(plan_);
    next->cancelled = std::move(pred);
    return next;
}

ScanPlan rollup_root(const ScanPlan& plan_, std::string dir) {
    auto next = clone(plan_);
    next->rollup_root = std::move(dir);
    return next;
}

ScanPlan views_root(const ScanPlan& plan_, std::string dir) {
    auto next = clone(plan_);
    next->views_root = std::move(dir);
    return next;
}

ScanPlan emit_all_metadata(const ScanPlan& plan_, bool v) {
    auto next = clone(plan_);
    next->emit_all_metadata = v;
    return next;
}

coro::CoroTask<ExportStats> for_each_batch(
    ScanPlan plan_,
    std::function<void(std::size_t, const std::vector<std::string_view>&)>
        on_batch,
    std::size_t num_slots, std::uint64_t limit) {
    co_return co_await detail::run_scan_batches(plan_, num_slots, limit,
                                                on_batch);
}

bool export_would_bootstrap(const ScanPlan& plan_) {
    return detail::export_bootstrap_eligible(*plan_);
}

bool collect_would_bootstrap(const ScanPlan& plan_) {
    return detail::collect_bootstrap_eligible(*plan_);
}

coro::CoroTask<ExportStats> export_json(ScanPlan plan_, ExportSink& sink) {
    co_return co_await detail::run_export(*plan_, sink);
}

coro::CoroTask<ExportStats> export_trace(ScanPlan plan_,
                                         TraceWriteOptions opts) {
    co_return co_await detail::run_export_trace(*plan_, opts);
}

dataframe::LazyFrame collect(const ScanPlan& plan_) {
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
            std::make_shared<ViewSource>(ScanPlan(std::move(stripped))))
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

coro::AsyncGenerator<dataframe::DataFrame> stream(ScanPlan plan_,
                                                  std::int64_t morsel_rows) {
    dataframe::LazyFrame lf = collect(plan_);
    auto gen = lf.stream(morsel_rows);
    while (auto df = co_await gen.next()) co_yield std::move(*df);
}

coro::CoroTask<dataframe::DataFrame> collect_frame(ScanPlan plan_) {
    // A row query (no group_by/agg) returns the matching events, not a count.
    if (detail::is_row_query(*plan_))
        co_return co_await detail::run_collect_rows(*plan_);
    // Every aggregation runs through the dataframe engine; materialize() opts
    // in to persisting its AggState partials as the rollup in the same pass.
    co_return detail::apply_agg_post_ops(
        co_await detail::run_collect_via_engine(*plan_), *plan_);
}

bool is_row_query(const ScanPlan& plan_) {
    return detail::is_row_query(*plan_);
}

coro::CoroTask<dataframe::DataFrame> call_tree(
    ScanPlan plan_, std::vector<std::string> partition, std::string ts,
    std::string dur, std::string name) {
    co_return co_await detail::run_call_tree(*plan_, std::move(partition),
                                             std::move(ts), std::move(dur),
                                             std::move(name));
}

coro::CoroTask<dataframe::DataFrame> flamegraph(
    ScanPlan plan_, std::vector<std::string> partition, std::string ts,
    std::string dur, std::string name, std::vector<std::string> group) {
    co_return co_await detail::run_flamegraph(
        *plan_, std::move(partition), std::move(ts), std::move(dur),
        std::move(name), std::move(group));
}

coro::CoroTask<std::pair<dataframe::DataFrame, dataframe::DataFrame>>
containment(ScanPlan plan_, std::vector<std::string> partition, std::string ts,
            std::string dur, std::string name, std::vector<std::string> group) {
    co_return co_await detail::run_containment(
        *plan_, std::move(partition), std::move(ts), std::move(dur),
        std::move(name), std::move(group));
}

coro::CoroTask<std::string> flamegraph_partial(
    ScanPlan plan_, std::vector<std::string> partition, std::string ts,
    std::string dur, std::string name, std::vector<std::string> group) {
    co_return co_await detail::run_flamegraph_partial(
        *plan_, std::move(partition), std::move(ts), std::move(dur),
        std::move(name), std::move(group));
}

dataframe::DataFrame merge_flamegraph_partials(
    const std::vector<std::string_view>& partials) {
    return detail::merge_flamegraph_partials(partials);
}

coro::CoroTask<ExportStats> run_folds(ScanPlan plan_,
                                      std::span<detail::Fold* const> folds,
                                      dftracer::utils::StringIntern& intern,
                                      detail::DynamicPrune* dyn_prune) {
    co_return co_await detail::run_folds(*plan_, folds, intern, dyn_prune);
}

coro::CoroTask<ExportStats> run(ScanPlan plan_, const ProgressFn* progress) {
    co_return co_await detail::run_materialize(*plan_, progress);
}

std::string materialize_dir(const ScanPlan& plan_) {
    return detail::materialize_view_dir(*plan_);
}

void register_materialized(const ScanPlan& plan_, const std::string& dir) {
    detail::register_view(dir, *plan_);
}

std::vector<std::string> mv_source(const ScanPlan& plan_) {
    std::vector<std::string> out;
    if (auto mv = detail::find_subsuming_view(*plan_))
        for (const auto& f : *mv) out.push_back(f.file_path);
    return out;
}

coro::CoroTask<TypedResult> collect_typed(ScanPlan plan_, int shard_begin,
                                          int shard_end,
                                          const ProgressFn* progress) {
    co_return co_await detail::run_collect_typed(*plan_, shard_begin, shard_end,
                                                 progress);
}

coro::CoroTask<ExportStats> export_counters(ScanPlan plan_, ExportSink& sink) {
    co_return co_await detail::run_export_counters(*plan_, sink);
}

coro::CoroTask<void> materialize_partials(
    ScanPlan plan_, const std::vector<std::string_view>& partials) {
    co_return co_await detail::run_materialize_partials(*plan_, partials);
}

std::optional<dataframe::DataFrame> reconstruct_if_cached(
    const ScanPlan& plan_) {
    return detail::run_reconstruct_if_cached(*plan_);
}

coro::CoroTask<std::string> aggregate_partial(ScanPlan plan_) {
    co_return co_await detail::run_aggregate_partial(*plan_);
}

dataframe::DataFrame merge_partials_to_table(
    const ScanPlan& plan_, const std::vector<std::string_view>& partials) {
    return detail::merge_partials_to_table(*plan_, partials);
}

ExportStats merge_counter_partials(
    const ScanPlan& plan_, const std::vector<std::string_view>& partials,
    ExportSink& sink) {
    return detail::merge_counters_partials(*plan_, partials, sink);
}

}  // namespace dftracer::utils::trace::views::detail::scan
