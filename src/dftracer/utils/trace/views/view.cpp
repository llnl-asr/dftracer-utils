#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/common/string_arena.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
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

namespace dftracer::utils::trace::views {

namespace scan = detail::scan;

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

Deferred<dataframe::DataFrame> ViewSession::collect(
    const std::shared_ptr<const detail::ViewPlan>& branch) {
    auto out = std::make_shared<dataframe::DataFrame>();
    const auto& bp = *branch;
    // Output key columns are [time_bucket?, group_by...], so a bucketed branch
    // has one more leading key column than its group_by (matches join layout).
    key_counts_.emplace_back(out.get(),
                             static_cast<std::int64_t>(bp.group_by.size()) +
                                 (bp.time_bucket_us > 0 ? 1 : 0));
    detail::BranchHooks h;
    h.agg =
        detail::AggBranch{bp.group_by,          bp.agg,  out,         branch,
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

Deferred<std::string> ViewSession::aggregate_partial(
    const std::shared_ptr<const detail::ViewPlan>& branch) {
    auto out =
        std::make_shared<dataframe::DataFrame>();  // unused; partial path
    auto partial = std::make_shared<std::string>();
    const auto& bp = *branch;
    detail::BranchHooks h;
    h.agg =
        detail::AggBranch{bp.group_by,          bp.agg,  out,         branch,
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

Deferred<dataframe::DataFrame> ViewSession::collect_events(
    const std::shared_ptr<const detail::ViewPlan>& branch) {
    const auto& bp = *branch;
    auto out = std::make_shared<dataframe::DataFrame>();
    // Per-worker owned events built straight into native columns (no Arrow);
    // each worker interns its own strings, so build_row_frame resolves them per
    // worker.
    struct Slot {
        dftracer::utils::StringIntern intern;
        std::vector<detail::FoldEvent> events;
    };
    auto slots = std::make_shared<std::vector<std::shared_ptr<Slot>>>();
    auto select = std::make_shared<std::vector<std::string>>(bp.select);
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
    const std::shared_ptr<const detail::ViewPlan>& branch,
    const std::vector<std::string>& partition, const std::string& ts,
    const std::string& dur, const std::string& name,
    const std::vector<std::string>& group,
    std::shared_ptr<dataframe::DataFrame> out_ct,
    std::shared_ptr<dataframe::DataFrame> out_fg,
    std::shared_ptr<std::string> out_partial) {
    const auto& bp = *branch;
    auto intern = std::make_shared<dftracer::utils::StringIntern>();
    auto spec =
        std::make_shared<detail::ContainmentSpec>(detail::make_containment_spec(
            *intern, partition, ts, dur, name, group));
    auto bufs = std::make_shared<
        std::vector<std::shared_ptr<std::vector<detail::ContainmentRow>>>>();
    const double time_scale = bp.time_scale;

    // One shared intern across workers keeps lane ids consistent, so the
    // per-worker rows concatenate without a re-key.
    auto make_consumer = [intern, spec, bufs]() {
        auto rows = std::make_shared<std::vector<detail::ContainmentRow>>();
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
    auto finalize = [intern, bufs, out_ct, out_fg, out_partial, time_scale]() {
        std::vector<detail::ContainmentRow> all;
        for (const auto& rows : *bufs)
            all.insert(all.end(), rows->begin(), rows->end());
        if (out_partial)
            *out_partial = detail::flamegraph_partial(all, *intern, time_scale);
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

    if (auto eq = detail::effective_query(bp))
        detail::add_fold_branch(*state_, std::move(*eq),
                                std::move(make_consumer), std::move(finalize));
    else
        detail::add_fold_branch(*state_, std::move(make_consumer),
                                std::move(finalize));
}

Deferred<dataframe::DataFrame> ViewSession::call_tree(
    const std::shared_ptr<const detail::ViewPlan>& branch,
    std::vector<std::string> partition, std::string ts, std::string dur,
    std::string name) {
    auto out = std::make_shared<dataframe::DataFrame>();
    add_containment_branch(branch, partition, ts, dur, name, {}, out, nullptr);
    return {out, executed_};
}

Deferred<dataframe::DataFrame> ViewSession::flamegraph(
    const std::shared_ptr<const detail::ViewPlan>& branch,
    std::vector<std::string> partition, std::string ts, std::string dur,
    std::string name) {
    auto out = std::make_shared<dataframe::DataFrame>();
    add_containment_branch(branch, partition, ts, dur, name, {}, nullptr, out);
    return {out, executed_};
}

ContainmentHandles ViewSession::containment(
    const std::shared_ptr<const detail::ViewPlan>& branch,
    std::vector<std::string> partition, std::string ts, std::string dur,
    std::string name) {
    auto out_ct = std::make_shared<dataframe::DataFrame>();
    auto out_fg = std::make_shared<dataframe::DataFrame>();
    add_containment_branch(branch, partition, ts, dur, name, {}, out_ct,
                           out_fg);
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

void TraceSession::add(const std::vector<dataframe::LazyFrame>& plans,
                       Resolve resolve) {
    if (*executed_)
        throw DFTUtilsException::cat(ErrorCode::INVALID_ARGUMENT,
                                     "TraceSession::collect after execute()");
    plans_.insert(plans_.end(), plans.begin(), plans.end());
    pending_.push_back(Pending{plans.size(), std::move(resolve)});
}

Deferred<dataframe::DataFrame> TraceSession::collect(
    dataframe::LazyFrame plan) {
    return collect(dataframe::detail::as_result(std::move(plan)));
}

Deferred<ExportStats> TraceSession::sink_json(const View& tv,
                                              ExportSink& sink) {
    return collect(tv.sink_json(sink, LAZY));
}

Deferred<ExportStats> TraceSession::materialize(const View& tv) {
    return collect(tv.materialize(LAZY));
}

coro::CoroTask<void> TraceSession::execute() {
    std::vector<dataframe::DataFrame> frames;
    if (!plans_.empty())
        frames = co_await dataframe::collect_all(std::move(plans_));
    plans_.clear();
    std::size_t at = 0;
    for (Pending& p : pending_) {
        std::vector<dataframe::DataFrame> mine(
            std::make_move_iterator(frames.begin() + at),
            std::make_move_iterator(frames.begin() + at + p.count));
        at += p.count;
        co_await p.resolve(std::move(mine));
    }
    pending_.clear();
    *executed_ = true;
}

namespace {

// The scan plan the source under a view's engine ops reads.
scan::ScanPlan scanned_plan(const dataframe::LazyFrame& lf) {
    return static_cast<const ViewSource&>(*dataframe::detail::plan_source(lf))
        .plan();
}

}  // namespace

View::View() : View(std::make_shared<detail::ViewPlan>()) {}

View::View(detail::ScanPlan plan) : plan_(), lf_(scan::collect(plan)) {
    plan_ = scanned_plan(lf_);
}

View::View(detail::ScanPlan plan, dataframe::LazyFrame lf)
    : plan_(std::move(plan)), lf_(std::move(lf)) {}

View View::from_file(std::string file_path, std::string index_path) {
    return View(scan::from_file(std::move(file_path), std::move(index_path)));
}

View View::from_files(std::vector<ViewFile> files,
                      indexing::BloomFilterCache* bloom_cache) {
    return View(scan::from_files(std::move(files), bloom_cache));
}

coro::CoroTask<View> View::from_directory(std::string dir,
                                          std::string index_path) {
    co_return View(
        co_await scan::from_directory(std::move(dir), std::move(index_path)));
}

View View::with_lazy(dataframe::LazyFrame lf) const {
    return View(plan_, std::move(lf));
}

View View::reshape(
    const char* builder,
    const std::function<scan::ScanPlan(const scan::ScanPlan&)>& step) const {
    auto refuse = [&](const std::string& why) {
        return DFTUtilsException::cat(
            ErrorCode::INVALID_ARGUMENT,
            std::string("View::") + builder + " " + why);
    };
    if (std::optional<std::string> op =
            dataframe::detail::first_non_filter_op(lf_))
        throw refuse("must come before '" + *op + "'");
    if (!dataframe::detail::first_op(lf_)) {
        scan::ScanPlan next = step(plan_);
        dataframe::LazyFrame lf =
            dataframe::detail::rebase(lf_, std::make_shared<ViewSource>(next));
        return View(std::move(next), std::move(lf));
    }
    // The filters before a builder select the events it sees, so they move
    // into the scan first.
    dataframe::LazyFrame planned = dataframe::detail::optimize_plan(lf_);
    std::optional<std::string> residual = dataframe::detail::first_op(planned);
    if (!residual) {
        const auto& src = static_cast<const ViewSource&>(
            *dataframe::detail::plan_source(planned));
        scan::ScanPlan next = step(src.plan());
        dataframe::LazyFrame lf = dataframe::detail::rebase(
            planned, std::make_shared<ViewSource>(next));
        return View(std::move(next), std::move(lf));
    }
    // A filter the scan cannot evaluate stays above it, which is sound only
    // while the builder leaves the columns and their values as they were.
    scan::ScanPlan next = step(plan_);
    if (std::string_view(builder) == "time_scale" ||
        ViewSource(next).names() != ViewSource(plan_).names())
        throw refuse("cannot follow '" + *residual +
                     "': the trace scan cannot evaluate that filter");
    dataframe::LazyFrame lf =
        dataframe::detail::rebase(lf_, std::make_shared<ViewSource>(next));
    return View(std::move(next), std::move(lf));
}

bool View::aggregates() const {
    const auto* src = dynamic_cast<const ViewSource*>(
        dataframe::detail::plan_source(dataframe::detail::optimize_plan(lf_))
            .get());
    return src && src->output() == TraceOutput::Events &&
           !detail::is_row_query(*src->plan());
}

scan::ScanPlan View::absorbed(const char* method, Need need) const {
    auto fail = [&](const std::string& why) {
        return DFTUtilsException::cat(
            ErrorCode::INVALID_ARGUMENT,
            std::string("View::") + method + " " + why);
    };
    dataframe::LazyFrame planned = dataframe::detail::optimize_plan(lf_);
    std::optional<std::pair<std::int64_t, std::int64_t>> window;
    if (need == Need::EventsWindow || need == Need::EventsHead)
        window = dataframe::detail::sole_slice(planned);
    if (std::optional<std::string> op = dataframe::detail::first_op(planned);
        op && !window)
        throw fail("cannot follow '" + *op + "'");
    const auto* src = dynamic_cast<const ViewSource*>(
        dataframe::detail::plan_source(planned).get());
    if (!src || src->output() != TraceOutput::Events)
        throw fail("needs a trace scan");
    scan::ScanPlan v = src->plan();
    const bool events = need == Need::Events || need == Need::EventsHead ||
                        need == Need::EventsWindow;
    if (window && need == Need::EventsHead && window->first != 0)
        throw fail("takes a head, not a window that skips rows");
    if (events && !detail::is_row_query(*v))
        throw fail("needs events, but the plan aggregates them");
    if (need == Need::Aggregate && detail::is_row_query(*v))
        throw fail("needs a group_by or agg");
    if (window) {
        const auto [offset, len] = *window;
        v = scan::limit(scan::offset(v, static_cast<std::uint64_t>(offset)),
                        len == std::numeric_limits<std::int64_t>::max()
                            ? 0
                            : static_cast<std::uint64_t>(len));
    }
    return v;
}

namespace {

dataframe::LazyFrame terminal_plan(scan::ScanPlan v, TraceOutput output,
                                   ContainmentArgs tree = {},
                                   std::shared_ptr<ExportSink> sink = nullptr) {
    const std::uint64_t budget = v->memory_budget;
    return dataframe::LazyFrame::scan(
               std::make_shared<ViewSource>(std::move(v), output,
                                            std::move(tree), std::move(sink)))
        .memory_budget(budget);
}

dataframe::LazyResult<std::string> partial_result(dataframe::LazyFrame plan) {
    return {
        {std::move(plan)}, [](std::vector<dataframe::DataFrame> frames) {
            return dataframe::detail::ready(detail::partial_of(frames.front()));
        }};
}

dataframe::LazyResult<ExportStats> stats_result(dataframe::LazyFrame plan) {
    return {
        {std::move(plan)}, [](std::vector<dataframe::DataFrame> frames) {
            return dataframe::detail::ready(detail::stats_of(frames.front()));
        }};
}

coro::CoroTask<TypedResult> run_typed(scan::ScanPlan v, int shard_begin,
                                      int shard_end, ProgressFn progress) {
    co_return co_await scan::collect_typed(v, shard_begin, shard_end,
                                           progress ? &progress : nullptr);
}

coro::CoroTask<ExportStats> run_materialize(scan::ScanPlan v,
                                            ProgressFn progress) {
    co_return co_await scan::run(v, progress ? &progress : nullptr);
}

coro::CoroTask<void> materialize_from_partials(
    scan::ScanPlan v, std::vector<std::string_view> partials) {
    co_await detail::run_materialize_partials(*v, partials);
}

coro::CoroTask<ExportStats> scan_batches(
    scan::ScanPlan v,
    std::function<void(std::size_t, const std::vector<std::string_view>&)>
        on_batch,
    std::size_t num_slots, std::uint64_t limit) {
    const std::uint64_t cap = v->limit;
    co_return co_await scan::for_each_batch(
        v, std::move(on_batch), num_slots,
        cap && limit ? std::min(cap, limit) : (cap ? cap : limit));
}

coro::CoroTask<ExportStats> scan_folds(scan::ScanPlan v,
                                       std::span<detail::Fold* const> folds,
                                       dftracer::utils::StringIntern& intern) {
    co_return co_await scan::run_folds(v, folds, intern);
}

coro::CoroTask<ExportStats> run_sink_counters(scan::ScanPlan v,
                                              ExportSink& sink) {
    co_return co_await scan::export_counters(v, sink);
}

coro::CoroTask<ExportStats> run_sink_trace(scan::ScanPlan v,
                                           TraceWriteOptions opts) {
    co_return co_await scan::export_trace(v, std::move(opts));
}

}  // namespace

dataframe::LazyFrame View::call_tree(std::vector<std::string> partition,
                                     std::string ts, std::string dur,
                                     std::string name) const {
    return terminal_plan(absorbed("call_tree", Need::Events),
                         TraceOutput::CallTree,
                         {std::move(partition),
                          std::move(ts),
                          std::move(dur),
                          std::move(name),
                          {}});
}

dataframe::LazyFrame View::flamegraph(std::vector<std::string> partition,
                                      std::string ts, std::string dur,
                                      std::string name,
                                      std::vector<std::string> group) const {
    return terminal_plan(absorbed("flamegraph", Need::Events),
                         TraceOutput::Flamegraph,
                         {std::move(partition), std::move(ts), std::move(dur),
                          std::move(name), std::move(group)});
}

dataframe::LazyResult<ContainmentResult> View::containment(
    std::vector<std::string> partition, std::string ts, std::string dur,
    std::string name, std::vector<std::string> group) const {
    scan::ScanPlan v = absorbed("containment", Need::Events);
    ContainmentArgs tree{std::move(partition), std::move(ts), std::move(dur),
                         std::move(name), std::move(group)};
    return {{terminal_plan(v, TraceOutput::CallTree, tree),
             terminal_plan(v, TraceOutput::Flamegraph, tree)},
            [](std::vector<dataframe::DataFrame> frames) {
                return dataframe::detail::ready(ContainmentResult{
                    std::move(frames[0]), std::move(frames[1])});
            }};
}

dataframe::LazyResult<std::string> View::flamegraph_partial(
    std::vector<std::string> partition, std::string ts, std::string dur,
    std::string name, std::vector<std::string> group) const {
    return partial_result(
        terminal_plan(absorbed("flamegraph_partial", Need::EventsHead),
                      TraceOutput::FlamegraphPartial,
                      {std::move(partition), std::move(ts), std::move(dur),
                       std::move(name), std::move(group)}));
}

dataframe::LazyResult<std::string> View::aggregate_partial() const {
    return partial_result(
        terminal_plan(absorbed("aggregate_partial", Need::Aggregate),
                      TraceOutput::AggregatePartial));
}

dataframe::DataFrame View::merge_flamegraph_partials(
    const std::vector<std::string_view>& partials) {
    return scan::merge_flamegraph_partials(partials);
}

dataframe::DataFrame View::merge_partials(
    const std::vector<std::string_view>& partials) const {
    return scan::merge_partials_to_table(
        absorbed("merge_partials", Need::Aggregate), partials);
}

dataframe::LazyResult<TypedResult> View::typed(int shard_begin, int shard_end,
                                               ProgressFn progress) const {
    scan::ScanPlan v = absorbed("typed", Need::Any);
    return {
        {},
        [v = std::move(v), shard_begin, shard_end,
         progress = std::move(progress)](std::vector<dataframe::DataFrame>) {
            return run_typed(v, shard_begin, shard_end, progress);
        }};
}

coro::CoroTask<TypedResult> View::collect_typed(int shard_begin, int shard_end,
                                                ProgressFn progress) const {
    return run_typed(absorbed("collect_typed", Need::Any), shard_begin,
                     shard_end, std::move(progress));
}

coro::CoroTask<ExportStats> View::sink_json(ExportSink& sink) const {
    return sink_json(sink, LAZY).collect();
}

dataframe::LazyResult<ExportStats> View::sink_json(ExportSink& sink,
                                                   Lazy) const {
    return sink_json(
        std::shared_ptr<ExportSink>(std::shared_ptr<void>(), &sink), LAZY);
}

dataframe::LazyResult<ExportStats> View::sink_json(
    std::shared_ptr<ExportSink> sink, Lazy) const {
    return stats_result(terminal_plan(absorbed("sink_json", Need::EventsWindow),
                                      TraceOutput::ExportJson, {},
                                      std::move(sink)));
}

coro::CoroTask<ExportStats> View::sink_trace(TraceWriteOptions opts) const {
    return sink_trace(std::move(opts), LAZY).collect();
}

dataframe::LazyResult<ExportStats> View::sink_trace(TraceWriteOptions opts,
                                                    Lazy) const {
    scan::ScanPlan v = absorbed("sink_trace", Need::EventsWindow);
    return {{},
            [v = std::move(v),
             opts = std::move(opts)](std::vector<dataframe::DataFrame>) {
                return run_sink_trace(v, opts);
            }};
}

coro::CoroTask<ExportStats> View::sink_counters(ExportSink& sink) const {
    return run_sink_counters(absorbed("sink_counters", Need::Aggregate), sink);
}

dataframe::LazyFrame View::compare(const View& variant) const {
    const scan::ScanPlan base = absorbed("compare", Need::Aggregate);
    const detail::ViewPlan& p = *base;
    const scan::ScanPlan other = scan::agg(
        scan::group_by(variant.absorbed("compare", Need::Events), p.group_by),
        p.agg);
    return View(base).lazy().compare_agg(
        View(other).lazy(), static_cast<std::int64_t>(p.group_by.size()));
}

coro::CoroTask<ExportStats> View::for_each_batch(
    std::function<void(std::size_t, const std::vector<std::string_view>&)>
        on_batch,
    std::size_t num_slots, std::uint64_t limit) const {
    return scan_batches(absorbed("for_each_batch", Need::EventsHead),
                        std::move(on_batch), num_slots, limit);
}

coro::CoroTask<ExportStats> View::run_folds(
    std::span<detail::Fold* const> folds,
    dftracer::utils::StringIntern& intern) const {
    return scan_folds(absorbed("run_folds", Need::EventsHead), folds, intern);
}

std::vector<TraceConfig> View::config() const { return scan::config(plan_); }

std::vector<std::string> View::columns() const { return scan::columns(plan_); }

std::vector<ColumnInfo> View::column_info() const {
    return scan::schema(plan_);
}

TimeMetric View::time_metric() const { return scan::time_metric(plan_); }

std::uint64_t View::memory_budget_bytes() const { return plan_->memory_budget; }

bool View::filters_events() const {
    return detail::is_row_query(*plan_) &&
           !dataframe::detail::first_non_filter_op(lf_);
}

bool View::export_would_bootstrap() const {
    return scan::export_would_bootstrap(
        absorbed("export_would_bootstrap", Need::EventsWindow));
}

bool View::collect_would_bootstrap() const {
    return scan::collect_would_bootstrap(
        absorbed("collect_would_bootstrap", Need::Any));
}

ExportStats View::merge_counter_partials(
    const std::vector<std::string_view>& partials, ExportSink& sink) const {
    return scan::merge_counter_partials(
        absorbed("merge_counter_partials", Need::Aggregate), partials, sink);
}

dataframe::LazyFrame View::branch(SessionBranch attach) const {
    scan::ScanPlan v = absorbed("branch", Need::EventsHead);
    const std::uint64_t budget = v->memory_budget;
    return dataframe::LazyFrame::scan(
               std::make_shared<ViewSource>(std::move(v), std::move(attach)))
        .memory_budget(budget);
}

coro::CoroTask<void> View::materialize_partials(
    const std::vector<std::string_view>& partials) const {
    return materialize_from_partials(
        absorbed("materialize_partials", Need::Aggregate), partials);
}

std::optional<dataframe::DataFrame> View::reconstruct_if_cached() const {
    return scan::reconstruct_if_cached(
        absorbed("reconstruct_if_cached", Need::Aggregate));
}

coro::CoroTask<ExportStats> View::materialize(std::uint64_t checkpoint_size,
                                              std::uint64_t part_size,
                                              ProgressFn progress) const {
    return materialize(LAZY, checkpoint_size, part_size, std::move(progress))
        .collect();
}

dataframe::LazyResult<ExportStats> View::materialize(
    Lazy, std::uint64_t checkpoint_size, std::uint64_t part_size,
    ProgressFn progress) const {
    scan::ScanPlan v = scan::materialize(absorbed("materialize", Need::Any),
                                         checkpoint_size, part_size);
    return {{},
            [v = std::move(v), progress = std::move(progress)](
                std::vector<dataframe::DataFrame>) {
                return run_materialize(v, progress);
            }};
}

std::vector<std::string> View::mv_source() const {
    return scan::mv_source(absorbed("mv_source", Need::Any));
}

std::string View::materialize_dir() const {
    return scan::materialize_dir(absorbed("materialize_dir", Need::Any));
}

void View::register_materialized(const std::string& dir) const {
    scan::register_materialized(absorbed("register_materialized", Need::Any),
                                dir);
}

View View::select(std::vector<std::string> names) const {
    if (detail::is_row_query(*plan_) &&
        !dataframe::detail::first_non_filter_op(lf_))
        return reshape("select", [&](const scan::ScanPlan& v) {
            return scan::select(v, std::move(names));
        });
    return LazyOps<View>::select(std::move(names));
}

View View::filter(Query q) const {
    return reshape("filter", [&](const scan::ScanPlan& v) {
        return scan::filter(v, std::move(q));
    });
}

View View::filter(const FieldExpr& pred) const {
    return reshape("filter", [&](const scan::ScanPlan& v) {
        return scan::filter(v, pred.to_query());
    });
}

View View::query(const std::string& dsl) const {
    return reshape(
        "query", [&](const scan::ScanPlan& v) { return scan::query(v, dsl); });
}

View View::phase(Phase p) const {
    return reshape("phase",
                   [&](const scan::ScanPlan& v) { return scan::phase(v, p); });
}

View View::time_range(double begin, double end) const {
    return reshape("time_range", [&](const scan::ScanPlan& v) {
        return scan::time_range(v, begin, end);
    });
}

View View::time_bucket(std::uint64_t interval_us) const {
    return reshape("time_bucket", [&](const scan::ScanPlan& v) {
        return scan::time_bucket(v, interval_us);
    });
}

View View::time_bucket(std::uint64_t interval_us,
                       std::uint64_t origin_us) const {
    return reshape("time_bucket", [&](const scan::ScanPlan& v) {
        return scan::time_bucket(v, interval_us, origin_us);
    });
}

View View::time_bucket_min(std::uint64_t interval_us) const {
    return reshape("time_bucket_min", [&](const scan::ScanPlan& v) {
        return scan::time_bucket_min(v, interval_us);
    });
}

View View::resolution(std::uint64_t cell_us) const {
    return reshape("resolution", [&](const scan::ScanPlan& v) {
        return scan::occ_cell(v, cell_us);
    });
}

View View::time_scale(double ns_ratio) const {
    return reshape("time_scale", [&](const scan::ScanPlan& v) {
        return scan::time_scale(v, ns_ratio);
    });
}

View View::group_by(std::vector<GroupKey> keys) const {
    return reshape("group_by", [&](const scan::ScanPlan& v) {
        return scan::group_by(v, std::move(keys));
    });
}

View View::agg(std::vector<AggSpec> specs) const {
    return reshape("agg", [&](const scan::ScanPlan& v) {
        return scan::agg(v, std::move(specs));
    });
}

View View::agg(std::vector<FieldAggExpr> exprs) const {
    return reshape("agg", [&](const scan::ScanPlan& v) {
        return scan::agg(v, std::move(exprs));
    });
}

View View::agg_numeric_args() const {
    return reshape("agg_numeric_args", [&](const scan::ScanPlan& v) {
        return scan::agg_numeric_args(v);
    });
}

View View::agg_numeric_args(std::vector<AggSpec> reductions) const {
    return reshape("agg_numeric_args", [&](const scan::ScanPlan& v) {
        return scan::agg_numeric_args(v, std::move(reductions));
    });
}

View View::metadata(bool include) const {
    return reshape("metadata", [&](const scan::ScanPlan& v) {
        return scan::metadata(v, include);
    });
}

View View::emit_all_metadata(bool v) const {
    return reshape("emit_all_metadata", [&](const scan::ScanPlan& base) {
        return scan::emit_all_metadata(base, v);
    });
}

View View::rollup_root(std::string dir) const {
    return reshape("rollup_root", [&](const scan::ScanPlan& v) {
        return scan::rollup_root(v, std::move(dir));
    });
}

View View::views_root(std::string dir) const {
    return reshape("views_root", [&](const scan::ScanPlan& v) {
        return scan::views_root(v, std::move(dir));
    });
}

View View::cancel_when(std::function<bool()> pred) const {
    return reshape("cancel_when", [&](const scan::ScanPlan& v) {
        return scan::cancel_when(v, std::move(pred));
    });
}

View View::memory_budget(std::uint64_t bytes) const {
    scan::ScanPlan next = scan::memory_budget(plan_, bytes);
    dataframe::LazyFrame lf =
        dataframe::detail::rebase(lf_, std::make_shared<ViewSource>(next))
            .memory_budget(bytes);
    return View(std::move(next), std::move(lf));
}

}  // namespace dftracer::utils::trace::views
