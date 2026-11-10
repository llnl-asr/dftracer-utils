// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/memory_budget.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/cache/mv_store.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/json/parser.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/comparator/compare_view.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/time_metric.h>
#include <dftracer/utils/trace/views/containment_fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/result_join.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/trace/views/view_source.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace dftracer::utils::trace::views {

ViewSession::ViewSession(dstd::SharedPtr<const detail::ViewPlan> plan)
    : state_(detail::make_view_session_state(std::move(plan))) {}

void ViewSession::attach_fold(
    dstd::Optional<Query> predicate,
    dstd::Function<dstd::Function<void(json::Value&, dstd::StringView)>()>
        make_consumer,
    dstd::Function<dstd::Result<void>()> finalize) {
    if (predicate)
        detail::add_fold_branch(*state_, std::move(*predicate),
                                std::move(make_consumer), std::move(finalize));
    else
        detail::add_selection_fold_branch(*state_, std::move(make_consumer),
                                          std::move(finalize));
}

void ViewSession::attach_fold_factory(
    dstd::Function<
        dstd::UniquePtr<detail::Fold>(dftracer::utils::dstd::StringIntern&)>
        make,
    dstd::Function<dstd::Result<void>()> finalize) {
    detail::add_fold_factory(*state_, std::move(make), std::move(finalize));
}

dstd::Optional<Query> ViewSession::pushable(const FieldExpr& predicate) {
    auto query = predicate.to_duql();
    if (!query) {
        detail::keep_session_error(*state_, query.error());
        return dstd::nullopt;
    }
    return std::move(*query);
}

void ViewSession::propose_base_prune(Query q) {
    detail::propose_base_prune(*state_, std::move(q));
}

Deferred<dataframe::DataFrame> ViewSession::collect(
    Query predicate, dstd::Vector<GroupKey> group_by,
    dstd::Vector<AggSpec> agg) {
    auto out = dstd::make_shared<dataframe::DataFrame>();
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
    dstd::Vector<GroupKey> group_by, dstd::Vector<AggSpec> agg) {
    auto out = dstd::make_shared<dataframe::DataFrame>();
    key_counts_.emplace_back(out.get(),
                             static_cast<std::int64_t>(group_by.size()));
    // No predicate: match all scanned events. The descriptor lets execute()
    // serve this branch from a rollup or the tier instead of scanning.
    detail::BranchHooks h;
    h.agg = detail::AggBranch{
        std::move(group_by), std::move(agg), out, nullptr, false, nullptr,
        dstd::nullopt};
    detail::add_branch(*state_, std::move(h));
    return {out, executed_};
}

Deferred<dataframe::DataFrame> ViewSession::collect(
    const dstd::SharedPtr<const detail::ViewPlan>& branch) {
    auto out = dstd::make_shared<dataframe::DataFrame>();
    const auto& bp = *branch;
    // Output key columns are [time_bucket?, group_by...], so a bucketed branch
    // has one more leading key column than its group_by (matches join layout).
    key_counts_.emplace_back(out.get(),
                             static_cast<std::int64_t>(bp.group_by.size()) +
                                 (bp.time_bucket_us > 0 ? 1 : 0));
    detail::BranchHooks h;
    h.agg =
        detail::AggBranch{bp.group_by,          bp.agg,  out,          branch,
                          bp.query.has_value(), nullptr, dstd::nullopt};
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
        right.executed_ != executed_) {
        detail::keep_session_error(
            *state_,
            dstd::Error::from_format<
                "ViewSession::join: both handles must be collect() results of "
                "this session">(common::ErrorCode::InvalidArgument));
        return {};
    }
    if (n_key < 0) n_key = key_count_of(left.value_.get());
    if (n_key < 0) {
        detail::keep_session_error(
            *state_,
            dstd::Error::from_format<
                "ViewSession::join: cannot infer n_key; pass it explicitly">(
                common::ErrorCode::InvalidArgument));
        return {};
    }
    auto out = dstd::make_shared<dataframe::DataFrame>();
    combines_.emplace_back([out, l = left.value_, r = right.value_, n_key,
                            how]() -> dstd::Result<void> {
        auto joined = join_batches(*l, *r, n_key, how);
        if (!joined) return joined.error();
        *out = std::move(*joined);
        return {};
    });
    return {out, executed_};
}

Deferred<dataframe::DataFrame> ViewSession::compare(
    Deferred<dataframe::DataFrame> baseline,
    Deferred<dataframe::DataFrame> variant, std::int64_t n_key) {
    if (!baseline.value_ || !variant.value_ ||
        baseline.executed_ != executed_ || variant.executed_ != executed_) {
        detail::keep_session_error(
            *state_,
            dstd::Error::from_format<
                "ViewSession::compare: both handles must be collect() results "
                "of this session">(common::ErrorCode::InvalidArgument));
        return {};
    }
    if (n_key < 0) n_key = key_count_of(baseline.value_.get());
    if (n_key < 0) {
        detail::keep_session_error(
            *state_,
            dstd::Error::from_format<
                "ViewSession::compare: cannot infer n_key; pass it explicitly">(
                common::ErrorCode::InvalidArgument));
        return {};
    }
    auto out = dstd::make_shared<dataframe::DataFrame>();
    combines_.emplace_back([out, b = baseline.value_, v = variant.value_,
                            n_key]() -> dstd::Result<void> {
        auto compared = comparator::CompareView::compare_batches(*b, *v, n_key);
        if (!compared) return compared.error();
        *out = std::move(*compared);
        return {};
    });
    return {out, executed_};
}

Deferred<dstd::String> ViewSession::aggregate_partial(
    const dstd::SharedPtr<const detail::ViewPlan>& branch) {
    auto out =
        dstd::make_shared<dataframe::DataFrame>();  // unused; partial path
    auto partial = dstd::make_shared<dstd::String>();
    const auto& bp = *branch;
    detail::BranchHooks h;
    h.agg =
        detail::AggBranch{bp.group_by,          bp.agg,  out,          branch,
                          bp.query.has_value(), partial, dstd::nullopt};
    detail::add_branch(*state_, std::move(h));
    return {partial, executed_};
}

void ViewSession::materialize(dstd::Vector<GroupKey> group_by,
                              dstd::Vector<AggSpec> agg) {
    detail::add_materialize_branch(*state_, std::move(group_by),
                                   std::move(agg));
}

Deferred<ExportStats> ViewSession::export_json(Query predicate,
                                               ExportSink& sink) {
    auto out = dstd::make_shared<ExportStats>();
    detail::BranchHooks h = detail::make_export_branch(sink, out);
    h.predicate = std::move(predicate);
    detail::add_branch(*state_, std::move(h));
    return {out, executed_};
}

Deferred<ExportStats> ViewSession::export_json(ExportSink& sink) {
    auto out = dstd::make_shared<ExportStats>();
    detail::BranchHooks h = detail::make_export_branch(sink, out);
    detail::add_branch(*state_, std::move(h));
    return {out, executed_};
}

Deferred<dataframe::DataFrame> ViewSession::collect_events(
    const dstd::SharedPtr<const detail::ViewPlan>& branch) {
    const auto& bp = *branch;
    auto out = dstd::make_shared<dataframe::DataFrame>();
    // Per-worker owned events built straight into native columns (no Arrow);
    // each worker interns its own strings, so build_row_frame resolves them per
    // worker.
    // finalize frees a slot's events as soon as its frame is built; the
    // frame's strings keep the intern chunks they view.
    struct Slot {
        Slot()
            : intern(dstd::make_unique<dftracer::utils::dstd::StringIntern>()),
              cache(dstd::make_unique<dftracer::utils::dstd::InternCache>(
                  *intern)),
              events(dstd::make_unique<detail::FoldEventBuffer>()) {}
        dstd::UniquePtr<dftracer::utils::dstd::StringIntern> intern;
        dstd::UniquePtr<dftracer::utils::dstd::InternCache> cache;
        dstd::UniquePtr<detail::FoldEventBuffer> events;
    };
    auto slots = dstd::make_shared<dstd::Vector<dstd::SharedPtr<Slot>>>();
    auto select = dstd::make_shared<dstd::Vector<dstd::String>>(bp.select);
    const double time_scale = bp.time_scale;
    const bool by_path = detail::plan_by_path(bp);
    const bool keep_metadata = detail::metadata_rows(bp);
    const dftracer::utils::index::RecordSchema* record_schema =
        by_path && !detail::plan_record_schema(bp).fields.empty()
            ? &detail::plan_record_schema(bp)
            : nullptr;

    auto make_consumer = [slots, by_path, keep_metadata, record_schema]() {
        auto slot = dstd::make_shared<Slot>();
        slots->push_back(slot);
        return [slot, by_path, keep_metadata, record_schema](json::Value& jv,
                                                             dstd::StringView) {
            detail::FoldEvent& fe = slot->events->append();
            if (by_path)
                detail::decode_record_into(fe, jv, *slot->intern,
                                           /*capture_schema=*/false, nullptr,
                                           record_schema, nullptr, 0, nullptr,
                                           false, slot->cache.get());
            else
                detail::extract_fold_event_into(fe, jv, *slot->intern,
                                                /*needs_args=*/true,
                                                slot->cache.get());
            if (fe.phase == RecordPhase::Unknown ||
                (!keep_metadata && fe.phase == RecordPhase::Metadata))
                slot->events->discard_last();
        };
    };
    auto finalize = [slots, select, out, time_scale,
                     by_path]() -> dstd::Result<void> {
        dstd::Vector<dataframe::DataFrame> frames;
        frames.reserve(slots->size());
        for (const auto& s : *slots) {
            if (!s->events->empty())
                frames.push_back(detail::build_row_frame(s->events->events(),
                                                         *s->intern, *select,
                                                         time_scale, by_path));
            s->cache.reset();
            s->events.reset();
            s->intern.reset();
        }
        if (frames.empty()) return {};  // out stays an empty frame
        dstd::Vector<const dataframe::DataFrame*> parts;
        parts.reserve(frames.size());
        for (const auto& f : frames) parts.push_back(&f);
        // Slots discover different args, so union their schemas.
        auto joined = dataframe::concat(parts, dataframe::ConcatHow::Diagonal);
        if (!joined) return joined.error();
        *out = std::move(*joined);
        return {};
    };

    // effective_query folds the branch's phase into the predicate (bp.query
    // alone drops it), so a branch's phase() filters in a fused session.
    auto eq = detail::effective_query(bp);
    if (!eq) {
        detail::keep_session_error(*state_, eq.error());
        return {};
    }
    if (*eq)
        detail::add_fold_branch(*state_, std::move(**eq),
                                std::move(make_consumer), std::move(finalize),
                                keep_metadata);
    else
        detail::add_fold_branch(*state_, std::move(make_consumer),
                                std::move(finalize), keep_metadata);
    return {out, executed_};
}

// One containment branch buffering rows once (shared intern for cross-slot lane
// consistency); finalize builds whichever of out_ct/out_fg is requested,
// sorting each lane once when both are. False, with the error kept, when the
// branch's query cannot be built.
bool ViewSession::add_containment_branch(
    const dstd::SharedPtr<const detail::ViewPlan>& branch,
    const dstd::Vector<dstd::String>& partition, const dstd::String& ts,
    const dstd::String& dur, const dstd::String& name,
    const dstd::Vector<dstd::String>& group,
    dstd::SharedPtr<dataframe::DataFrame> out_ct,
    dstd::SharedPtr<dataframe::DataFrame> out_fg,
    dstd::SharedPtr<dstd::String> out_partial) {
    const auto& bp = *branch;
    auto eq = detail::effective_query(bp);
    if (!eq) {
        detail::keep_session_error(*state_, eq.error());
        return false;
    }
    auto intern = dstd::make_shared<dftracer::utils::dstd::StringIntern>();
    auto spec = dstd::make_shared<detail::ContainmentSpec>(
        detail::make_containment_spec(*intern, partition, ts, dur, name, group,
                                      detail::plan_by_path(bp)
                                          ? &detail::plan_record_schema(bp)
                                          : nullptr,
                                      bp.time_scale));
    auto bufs = dstd::make_shared<
        dstd::Vector<dstd::SharedPtr<dstd::Vector<detail::ContainmentRow>>>>();
    const bool by_path = detail::plan_by_path(bp);
    const dftracer::utils::index::RecordSchema* record_schema =
        by_path && !detail::plan_record_schema(bp).fields.empty()
            ? &detail::plan_record_schema(bp)
            : nullptr;

    // One shared intern across workers keeps lane ids consistent, so the
    // per-worker rows concatenate without a re-key.
    auto make_consumer = [intern, spec, bufs, by_path, record_schema]() {
        auto rows = dstd::make_shared<dstd::Vector<detail::ContainmentRow>>();
        bufs->push_back(rows);
        auto cache =
            dstd::make_shared<dftracer::utils::dstd::InternCache>(*intern);
        auto scratch = dstd::make_shared<detail::FoldEvent>();
        return [intern, cache, spec, rows, scratch, by_path, record_schema](
                   json::Value& jv, dstd::StringView) {
            detail::FoldEvent& fe = *scratch;
            if (by_path)
                detail::decode_record_into(fe, jv, *intern,
                                           /*capture_schema=*/false, nullptr,
                                           record_schema, nullptr, 0, nullptr,
                                           false, cache.get());
            else
                detail::extract_fold_event_into(fe, jv, *intern,
                                                spec->needs_args, cache.get(),
                                                &spec->nested_captures);
            detail::ContainmentRow r;
            if (detail::containment_row(fe, *spec, *intern, r))
                rows->push_back(r);
        };
    };
    auto finalize = [intern, spec, bufs, out_ct, out_fg,
                     out_partial]() -> dstd::Result<void> {
        const detail::TimeOut& time_out = spec->out;
        dstd::Vector<detail::ContainmentRow> all;
        for (const auto& rows : *bufs)
            all.insert(all.end(), rows->begin(), rows->end());
        if (out_partial)
            *out_partial = detail::flamegraph_partial(all, *intern, time_out);
        if (out_ct && out_fg) {
            auto pr = detail::build_containment_both(all, *intern, time_out);
            *out_ct = std::move(pr.first);
            *out_fg = std::move(pr.second);
        } else if (out_ct) {
            *out_ct = detail::build_call_tree(all, detail::sorted_lanes(all),
                                              *intern, time_out);
        } else if (out_fg) {
            *out_fg = detail::build_flamegraph(all, detail::sorted_lanes(all),
                                               *intern, time_out);
        }
        return {};
    };

    if (*eq)
        detail::add_fold_branch(*state_, std::move(**eq),
                                std::move(make_consumer), std::move(finalize));
    else
        detail::add_fold_branch(*state_, std::move(make_consumer),
                                std::move(finalize));
    return true;
}

Deferred<dataframe::DataFrame> ViewSession::call_tree(
    const dstd::SharedPtr<const detail::ViewPlan>& branch,
    dstd::Vector<dstd::String> partition, dstd::String ts, dstd::String dur,
    dstd::String name) {
    auto out = dstd::make_shared<dataframe::DataFrame>();
    if (!add_containment_branch(branch, partition, ts, dur, name, {}, out,
                                nullptr))
        return {};
    return {out, executed_};
}

Deferred<dataframe::DataFrame> ViewSession::flamegraph(
    const dstd::SharedPtr<const detail::ViewPlan>& branch,
    dstd::Vector<dstd::String> partition, dstd::String ts, dstd::String dur,
    dstd::String name) {
    auto out = dstd::make_shared<dataframe::DataFrame>();
    if (!add_containment_branch(branch, partition, ts, dur, name, {}, nullptr,
                                out))
        return {};
    return {out, executed_};
}

ContainmentHandles ViewSession::containment(
    const dstd::SharedPtr<const detail::ViewPlan>& branch,
    dstd::Vector<dstd::String> partition, dstd::String ts, dstd::String dur,
    dstd::String name) {
    auto out_ct = dstd::make_shared<dataframe::DataFrame>();
    auto out_fg = dstd::make_shared<dataframe::DataFrame>();
    if (!add_containment_branch(branch, partition, ts, dur, name, {}, out_ct,
                                out_fg))
        return {};
    return {{out_ct, executed_}, {out_fg, executed_}};
}

coro::CoroTask<dstd::Result<ExportStats>> ViewSession::execute() {
    auto ran = co_await detail::run_session(state_);
    if (!ran) co_return ran.error();
    const ExportStats stats = *ran;
    // Branch outputs are populated; run the post-scan combines before flipping
    // executed_ so a combine reads the raw branch out, not a resolved handle.
    for (auto& c : combines_)
        if (auto combined = c(); !combined) co_return combined.error();
    *executed_ = true;
    co_return stats;
}

void TraceSession::add(const dstd::Vector<dataframe::LazyFrame>& plans,
                       Resolve resolve) {
    if (*executed_) dstd::fatal("TraceSession::collect after execute()");
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

coro::CoroTask<dstd::Result<void>> TraceSession::execute() {
    dstd::Vector<dataframe::DataFrame> frames;
    if (!plans_.empty()) {
        auto collected = co_await dataframe::collect_all(std::move(plans_));
        if (!collected) co_return collected.error();
        frames = std::move(*collected);
    }
    plans_.clear();
    std::size_t at = 0;
    for (Pending& p : pending_) {
        dstd::Vector<dataframe::DataFrame> mine(
            std::make_move_iterator(frames.begin() + at),
            std::make_move_iterator(frames.begin() + at + p.count));
        at += p.count;
        auto resolved = co_await p.resolve(std::move(mine));
        if (!resolved) co_return resolved.error();
    }
    pending_.clear();
    *executed_ = true;
    co_return dstd::Result<void>{};
}

namespace {

// The scan plan the source under a view's engine ops reads.
detail::scan::ScanPlan scanned_plan(const dataframe::LazyFrame& lf) {
    return static_cast<const ViewSource&>(*dataframe::detail::plan_source(lf))
        .plan();
}

}  // namespace

View::View() : View(dstd::make_shared<detail::ViewPlan>()) {}

View::View(detail::ScanPlan plan) : plan_(), lf_(detail::scan::collect(plan)) {
    plan_ = scanned_plan(lf_);
}

View::View(detail::ScanPlan plan, dataframe::LazyFrame lf)
    : plan_(std::move(plan)), lf_(std::move(lf)) {}

View View::from_file(dstd::String file_path, dstd::String index_path) {
    dstd::Vector<ViewFile> files;
    files.push_back(ViewFile{std::move(file_path), std::move(index_path)});
    return from_files(std::move(files));
}

View View::from_files(dstd::Vector<ViewFile> files) {
    auto plan = detail::scan::from_files(std::move(files));
    if (plan->error) return View().failed(*plan->error);
    return View(std::move(plan));
}

coro::CoroTask<View> View::from_directory(dstd::String dir,
                                          dstd::String index_path) {
    auto listing =
        detail::scan::from_directory(std::move(dir), std::move(index_path));
    detail::ScanPlan plan = co_await std::move(listing);
    if (plan->error) co_return View().failed(*plan->error);
    co_return View(std::move(plan));
}

dstd::Result<void> View::status() const { return lf_.status(); }

View View::failed(dstd::Error error) const {
    return View(plan_, dataframe::detail::fail_plan(lf_, std::move(error)));
}

View View::with_lazy(dataframe::LazyFrame lf) const {
    return View(plan_, std::move(lf));
}

View View::reshape(
    const char* builder,
    const dstd::Function<detail::scan::ScanPlan(const detail::scan::ScanPlan&)>&
        step) const {
    if (!lf_.status()) return *this;
    auto refuse = [&](const dstd::String& why) {
        return failed(dstd::Error::from_format<"View::{} {}">(
            common::ErrorCode::InvalidArgument, builder, why));
    };
    if (dstd::Optional<dstd::String> op =
            dataframe::detail::first_non_filter_op(lf_))
        return refuse(dstd::fmt<"must come before '{}'">(*op));
    if (!dataframe::detail::first_op(lf_)) {
        detail::ScanPlan next = step(plan_);
        if (next->error) return failed(*next->error);
        dataframe::LazyFrame lf =
            dataframe::detail::rebase(lf_, dstd::make_shared<ViewSource>(next));
        return View(std::move(next), std::move(lf));
    }
    // The filters before a builder select the events it sees, so they move
    // into the scan first.
    dataframe::LazyFrame planned = dataframe::detail::optimize_plan(lf_);
    dstd::Optional<dstd::String> residual =
        dataframe::detail::first_op(planned);
    if (!residual) {
        const auto& src = static_cast<const ViewSource&>(
            *dataframe::detail::plan_source(planned));
        detail::ScanPlan next = step(src.plan());
        if (next->error) return failed(*next->error);
        dataframe::LazyFrame lf = dataframe::detail::rebase(
            planned, dstd::make_shared<ViewSource>(next));
        return View(std::move(next), std::move(lf));
    }
    // A filter the scan cannot evaluate stays above it, which is sound only
    // while the builder leaves the columns and their values as they were.
    detail::ScanPlan next = step(plan_);
    if (next->error) return failed(*next->error);
    if (dstd::StringView(builder) == "time_scale" ||
        ViewSource(next).names() != ViewSource(plan_).names())
        return refuse(
            dstd::fmt<"cannot follow '{}': the trace scan cannot evaluate "
                      "that filter">(*residual));
    dataframe::LazyFrame lf =
        dataframe::detail::rebase(lf_, dstd::make_shared<ViewSource>(next));
    return View(std::move(next), std::move(lf));
}

bool View::aggregates() const {
    const auto* src = dynamic_cast<const ViewSource*>(
        dataframe::detail::plan_source(dataframe::detail::optimize_plan(lf_))
            .get());
    return src && src->output() == TraceOutput::Events &&
           !detail::is_row_query(*src->plan());
}

dstd::Result<detail::scan::ScanPlan> View::absorbed(const char* method,
                                                    Need need) const {
    if (auto kept = lf_.status(); !kept) return kept.error();
    auto fail = [&](const dstd::String& why) {
        return common::make_error<"View::{} {}">(
            common::ErrorCode::InvalidArgument, method, why);
    };
    dataframe::LazyFrame planned = dataframe::detail::optimize_plan(lf_);
    dstd::Optional<dstd::Pair<std::int64_t, std::int64_t>> window;
    if (need == Need::EventsWindow || need == Need::EventsHead)
        window = dataframe::detail::sole_slice(planned);
    if (dstd::Optional<dstd::String> op = dataframe::detail::first_op(planned);
        op && !window)
        return fail(dstd::fmt<"cannot follow '{}'">(*op));
    const auto* src = dynamic_cast<const ViewSource*>(
        dataframe::detail::plan_source(planned).get());
    if (!src || src->output() != TraceOutput::Events)
        return fail("needs a trace scan");
    detail::scan::ScanPlan v = src->plan();
    const bool events = need == Need::Events || need == Need::EventsHead ||
                        need == Need::EventsWindow;
    if (window && need == Need::EventsHead && window->first != 0)
        return fail("takes a head, not a window that skips rows");
    if (events && !detail::is_row_query(*v))
        return fail("needs events, but the plan aggregates them");
    if (need == Need::Aggregate && detail::is_row_query(*v))
        return fail("needs a group_by or agg");
    if (window) {
        const auto [offset, len] = *window;
        v = detail::scan::limit(
            detail::scan::offset(v, static_cast<std::uint64_t>(offset)),
            len == std::numeric_limits<std::int64_t>::max()
                ? 0
                : static_cast<std::uint64_t>(len));
    }
    return v;
}

namespace {

dataframe::LazyFrame terminal_plan(detail::scan::ScanPlan v, TraceOutput output,
                                   ContainmentArgs tree = {},
                                   dstd::SharedPtr<ExportSink> sink = nullptr) {
    const std::uint64_t budget = v->memory_budget;
    return dataframe::LazyFrame::scan(
               dstd::make_shared<ViewSource>(std::move(v), output,
                                             std::move(tree), std::move(sink)))
        .memory_budget(budget);
}

dataframe::LazyResult<dstd::String> partial_result(dataframe::LazyFrame plan) {
    return {
        {std::move(plan)}, [](dstd::Vector<dataframe::DataFrame> frames) {
            return dataframe::detail::ready(detail::partial_of(frames.front()));
        }};
}

dataframe::LazyResult<ExportStats> stats_result(dataframe::LazyFrame plan) {
    return {
        {std::move(plan)}, [](dstd::Vector<dataframe::DataFrame> frames) {
            return dataframe::detail::ready(detail::stats_of(frames.front()));
        }};
}

// A result whose one plan holds `error`, so collecting it ends with the error.
template <class T>
dataframe::LazyResult<T> failed_result(const dataframe::LazyFrame& lf,
                                       dstd::Error error) {
    return {{dataframe::detail::fail_plan(lf, std::move(error))},
            [](dstd::Vector<dataframe::DataFrame>) {
                return dataframe::detail::ready(T{});
            }};
}

template <class T>
coro::CoroTask<dstd::Result<T>> failed_task(dstd::Error error) {
    co_return std::move(error);
}

// The plan of a containment terminal: the absorbed `v` with its duration role
// and timing.
dstd::Result<detail::scan::ScanPlan> timed_plan(
    dstd::Result<detail::scan::ScanPlan> v, dstd::StringView op) {
    if (!v) return v.error();
    if (auto role = detail::require_role(**v, op, detail::TraceRole::Duration);
        !role)
        return role.error();
    return detail::scan::timed(*v);
}

coro::CoroTask<dstd::Result<TypedResult>> run_typed(
    detail::scan::ScanPlan v, int shard_begin, int shard_end,
    dstd::SharedPtr<ProgressFn> progress) {
    auto typed = detail::scan::collect_typed(
        v, shard_begin, shard_end,
        progress && *progress ? progress.get() : nullptr);
    co_return co_await std::move(typed);
}

coro::CoroTask<dstd::Result<ExportStats>> run_materialize(
    detail::scan::ScanPlan v, dstd::SharedPtr<ProgressFn> progress) {
    auto ran =
        detail::scan::run(v, progress && *progress ? progress.get() : nullptr);
    co_return co_await std::move(ran);
}

coro::CoroTask<dstd::Result<void>> materialize_from_partials(
    detail::scan::ScanPlan v, dstd::Vector<dstd::StringView> partials) {
    auto materialized = detail::run_materialize_partials(*v, partials);
    co_return co_await std::move(materialized);
}

coro::CoroTask<dstd::Result<ExportStats>> scan_batches(
    detail::scan::ScanPlan v,
    dstd::Function<void(std::size_t, const dstd::Vector<dstd::StringView>&)>
        on_batch,
    std::size_t slot_count, std::uint64_t limit) {
    const std::uint64_t cap = v->limit;
    auto scanned = detail::scan::for_each_batch(
        v, std::move(on_batch), slot_count,
        cap && limit ? std::min(cap, limit) : (cap ? cap : limit));
    co_return co_await std::move(scanned);
}

coro::CoroTask<dstd::Result<ExportStats>> scan_folds(
    detail::scan::ScanPlan v, dstd::Span<detail::Fold* const> folds,
    dftracer::utils::dstd::StringIntern& intern) {
    auto ran = detail::scan::run_folds(v, folds, intern);
    co_return co_await std::move(ran);
}

coro::CoroTask<dstd::Result<ExportStats>> run_sink_counters(
    detail::scan::ScanPlan v, ExportSink& sink) {
    auto ran = detail::scan::export_counters(v, sink);
    co_return co_await std::move(ran);
}

coro::CoroTask<dstd::Result<ExportStats>> run_sink_trace(
    detail::scan::ScanPlan v, TraceWriteOptions opts) {
    auto ran = detail::scan::export_trace(v, std::move(opts));
    co_return co_await std::move(ran);
}

}  // namespace

dataframe::LazyFrame View::call_tree(dstd::Vector<dstd::String> partition,
                                     dstd::String ts, dstd::String dur,
                                     dstd::String name) const {
    auto v = timed_plan(absorbed("call_tree", Need::Events), "call_tree");
    if (!v) return dataframe::detail::fail_plan(lf_, v.error());
    return terminal_plan(*v, TraceOutput::CallTree,
                         {std::move(partition),
                          std::move(ts),
                          std::move(dur),
                          std::move(name),
                          {}});
}

dataframe::LazyFrame View::flamegraph(dstd::Vector<dstd::String> partition,
                                      dstd::String ts, dstd::String dur,
                                      dstd::String name,
                                      dstd::Vector<dstd::String> group) const {
    auto v = timed_plan(absorbed("flamegraph", Need::Events), "flamegraph");
    if (!v) return dataframe::detail::fail_plan(lf_, v.error());
    return terminal_plan(*v, TraceOutput::Flamegraph,
                         {std::move(partition), std::move(ts), std::move(dur),
                          std::move(name), std::move(group)});
}

dataframe::LazyResult<ContainmentResult> View::containment(
    dstd::Vector<dstd::String> partition, dstd::String ts, dstd::String dur,
    dstd::String name, dstd::Vector<dstd::String> group) const {
    auto v = timed_plan(absorbed("containment", Need::Events), "containment");
    if (!v) return failed_result<ContainmentResult>(lf_, v.error());
    ContainmentArgs tree{std::move(partition), std::move(ts), std::move(dur),
                         std::move(name), std::move(group)};
    return {{terminal_plan(*v, TraceOutput::CallTree, tree),
             terminal_plan(*v, TraceOutput::Flamegraph, tree)},
            [](dstd::Vector<dataframe::DataFrame> frames) {
                return dataframe::detail::ready(ContainmentResult{
                    std::move(frames[0]), std::move(frames[1])});
            }};
}

dataframe::LazyResult<dstd::String> View::flamegraph_partial(
    dstd::Vector<dstd::String> partition, dstd::String ts, dstd::String dur,
    dstd::String name, dstd::Vector<dstd::String> group) const {
    auto v = timed_plan(absorbed("flamegraph_partial", Need::EventsHead),
                        "flamegraph");
    if (!v) return failed_result<dstd::String>(lf_, v.error());
    return partial_result(
        terminal_plan(*v, TraceOutput::FlamegraphPartial,
                      {std::move(partition), std::move(ts), std::move(dur),
                       std::move(name), std::move(group)}));
}

dataframe::LazyResult<dstd::String> View::aggregate_partial() const {
    auto v = absorbed("aggregate_partial", Need::Aggregate);
    if (!v) return failed_result<dstd::String>(lf_, v.error());
    return partial_result(terminal_plan(*v, TraceOutput::AggregatePartial));
}

dataframe::DataFrame View::merge_flamegraph_partials(
    const dstd::Vector<dstd::StringView>& partials) {
    return detail::scan::merge_flamegraph_partials(partials);
}

dstd::Result<dataframe::DataFrame> View::merge_partials(
    const dstd::Vector<dstd::StringView>& partials) const {
    auto v = absorbed("merge_partials", Need::Aggregate);
    if (!v) return v.error();
    return detail::scan::merge_partials_to_table(*v, partials);
}

dataframe::LazyResult<TypedResult> View::typed(int shard_begin, int shard_end,
                                               ProgressFn progress) const {
    auto v = absorbed("typed", Need::Any);
    if (!v) return failed_result<TypedResult>(lf_, v.error());
    return {{},
            [v = std::move(*v), shard_begin, shard_end,
             progress = dstd::make_shared<ProgressFn>(std::move(progress))](
                dstd::Vector<dataframe::DataFrame>) {
                return run_typed(v, shard_begin, shard_end, progress);
            }};
}

coro::CoroTask<dstd::Result<TypedResult>> View::collect_typed(
    int shard_begin, int shard_end, ProgressFn progress) const {
    auto v = absorbed("collect_typed", Need::Any);
    if (!v) return failed_task<TypedResult>(v.error());
    return run_typed(std::move(*v), shard_begin, shard_end,
                     dstd::make_shared<ProgressFn>(std::move(progress)));
}

coro::CoroTask<dstd::Result<ExportStats>> View::sink_json(
    ExportSink& sink) const {
    auto lazy = sink_json(sink, LAZY);
    auto collecting = lazy.collect();
    co_return co_await std::move(collecting);
}

dataframe::LazyResult<ExportStats> View::sink_json(ExportSink& sink,
                                                   Lazy) const {
    return sink_json(
        dstd::SharedPtr<ExportSink>(dstd::SharedPtr<void>(), &sink), LAZY);
}

dataframe::LazyResult<ExportStats> View::sink_json(
    dstd::SharedPtr<ExportSink> sink, Lazy) const {
    auto v = absorbed("sink_json", Need::EventsWindow);
    if (!v) return failed_result<ExportStats>(lf_, v.error());
    return stats_result(
        terminal_plan(*v, TraceOutput::ExportJson, {}, std::move(sink)));
}

coro::CoroTask<dstd::Result<ExportStats>> View::sink_trace(
    TraceWriteOptions opts) const {
    auto lazy = sink_trace(std::move(opts), LAZY);
    auto collecting = lazy.collect();
    co_return co_await std::move(collecting);
}

dataframe::LazyResult<ExportStats> View::sink_trace(TraceWriteOptions opts,
                                                    Lazy) const {
    auto v = absorbed("sink_trace", Need::EventsWindow);
    if (!v) return failed_result<ExportStats>(lf_, v.error());
    return {{},
            [v = std::move(*v),
             opts = std::move(opts)](dstd::Vector<dataframe::DataFrame>) {
                return run_sink_trace(v, opts);
            }};
}

coro::CoroTask<dstd::Result<ExportStats>> View::sink_counters(
    ExportSink& sink) const {
    auto v = absorbed("sink_counters", Need::Aggregate);
    if (!v) return failed_task<ExportStats>(v.error());
    return run_sink_counters(std::move(*v), sink);
}

dataframe::LazyFrame View::compare(const View& variant) const {
    auto base = absorbed("compare", Need::Aggregate);
    if (!base) return dataframe::detail::fail_plan(lf_, base.error());
    auto events = variant.absorbed("compare", Need::Events);
    if (!events) return dataframe::detail::fail_plan(lf_, events.error());
    const detail::ViewPlan& p = **base;
    const detail::scan::ScanPlan other =
        detail::scan::agg(detail::scan::group_by(*events, p.group_by), p.agg);
    return View(*base).lazy().compare_agg(
        View(other).lazy(), static_cast<std::int64_t>(p.group_by.size()));
}

coro::CoroTask<dstd::Result<ExportStats>> View::for_each_batch(
    dstd::Function<void(std::size_t, const dstd::Vector<dstd::StringView>&)>
        on_batch,
    std::size_t slot_count, std::uint64_t limit) const {
    auto v = absorbed("for_each_batch", Need::EventsHead);
    if (!v) return failed_task<ExportStats>(v.error());
    return scan_batches(std::move(*v), std::move(on_batch), slot_count, limit);
}

coro::CoroTask<dstd::Result<ExportStats>> View::run_folds(
    dstd::Span<detail::Fold* const> folds,
    dftracer::utils::dstd::StringIntern& intern) const {
    auto v = absorbed("run_folds", Need::EventsHead);
    if (!v) return failed_task<ExportStats>(v.error());
    return scan_folds(std::move(*v), folds, intern);
}

dstd::Result<dstd::Vector<TraceConfig>> View::config() const {
    return detail::scan::config(plan_);
}

dstd::Result<void> View::schema_status() const {
    return detail::resolve_record_schema(*plan_);
}

dstd::Result<dstd::Vector<dstd::String>> View::columns() const {
    if (auto kept = lf_.status(); !kept) return kept.error();
    if (auto ensured = detail::scan::ensure_catalog(plan_); !ensured)
        return ensured.error();
    return detail::scan::columns(plan_);
}

dstd::Result<dstd::Vector<ColumnInfo>> View::column_info() const {
    if (auto kept = lf_.status(); !kept) return kept.error();
    if (auto ensured = detail::scan::ensure_catalog(plan_); !ensured)
        return ensured.error();
    return detail::scan::schema(plan_);
}

dstd::Result<dstd::Vector<SchemaLeaf>> View::schema_tree() const {
    if (auto kept = lf_.status(); !kept) return kept.error();
    return detail::scan::schema_tree(plan_);
}

dstd::Result<dstd::Vector<SchemaLeaf>> detail::catalog_leaves(const View& v) {
    return detail::scan::catalog_tree(v.plan_);
}

dstd::String schema_tree_json(const dstd::Vector<SchemaLeaf>& leaves) {
    dstd::String out = "[";
    auto text = [&](const dstd::String& s) {
        if (s.empty()) {
            out += "null";
            return;
        }
        out += '"';
        dftracer::utils::json::append_json_escaped(out, s);
        out += '"';
    };
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        const SchemaLeaf& l = leaves[i];
        if (i) out += ',';
        out += "{\"path\":";
        text(l.path);
        out += ",\"type\":";
        text(l.type);
        out += ",\"count\":";
        out += l.count ? dstd::to_string(*l.count) : "null";
        out += ",\"field\":";
        text(l.field);
        out += ",\"declared_type\":";
        text(l.declared_type);
        out += '}';
    }
    out += ']';
    return out;
}

TimeMetric View::time_metric() const {
    return detail::scan::time_metric(plan_);
}

std::uint64_t View::memory_budget_bytes() const { return plan_->memory_budget; }

bool View::filters_events() const {
    return detail::is_row_query(*plan_) &&
           !dataframe::detail::first_non_filter_op(lf_);
}

dstd::Result<bool> View::export_would_bootstrap() const {
    auto v = absorbed("export_would_bootstrap", Need::EventsWindow);
    if (!v) return v.error();
    return detail::scan::export_would_bootstrap(*v);
}

dstd::Result<bool> View::collect_would_bootstrap() const {
    auto v = absorbed("collect_would_bootstrap", Need::Any);
    if (!v) return v.error();
    return detail::scan::collect_would_bootstrap(*v);
}

dstd::Result<ExportStats> View::merge_counter_partials(
    const dstd::Vector<dstd::StringView>& partials, ExportSink& sink) const {
    auto v = absorbed("merge_counter_partials", Need::Aggregate);
    if (!v) return v.error();
    return detail::scan::merge_counter_partials(*v, partials, sink);
}

dataframe::LazyFrame View::branch(SessionBranch attach) const {
    auto v = absorbed("branch", Need::EventsHead);
    if (!v) return dataframe::detail::fail_plan(lf_, v.error());
    const std::uint64_t budget = (*v)->memory_budget;
    return dataframe::LazyFrame::scan(
               dstd::make_shared<ViewSource>(std::move(*v), std::move(attach)))
        .memory_budget(budget);
}

coro::CoroTask<dstd::Result<void>> View::materialize_partials(
    const dstd::Vector<dstd::StringView>& partials) const {
    auto v = absorbed("materialize_partials", Need::Aggregate);
    if (!v) return failed_task<void>(v.error());
    return materialize_from_partials(std::move(*v), partials);
}

dstd::Result<dstd::Optional<dataframe::DataFrame>> View::reconstruct_if_cached()
    const {
    auto v = absorbed("reconstruct_if_cached", Need::Aggregate);
    if (!v) return v.error();
    return detail::scan::reconstruct_if_cached(*v);
}

coro::CoroTask<dstd::Result<ExportStats>> View::materialize(
    std::uint64_t checkpoint_size, std::uint64_t part_size,
    ProgressFn progress) const {
    auto lazy =
        materialize(LAZY, checkpoint_size, part_size, std::move(progress));
    auto collecting = lazy.collect();
    co_return co_await std::move(collecting);
}

dataframe::LazyResult<ExportStats> View::materialize(
    Lazy, std::uint64_t checkpoint_size, std::uint64_t part_size,
    ProgressFn progress) const {
    auto absorbed_plan = absorbed("materialize", Need::Any);
    if (!absorbed_plan)
        return failed_result<ExportStats>(lf_, absorbed_plan.error());
    detail::scan::ScanPlan v =
        detail::scan::materialize(*absorbed_plan, checkpoint_size, part_size);
    return {{},
            [v = std::move(v),
             progress = dstd::make_shared<ProgressFn>(std::move(progress))](
                dstd::Vector<dataframe::DataFrame>) {
                return run_materialize(v, progress);
            }};
}

dstd::Result<dstd::Vector<dstd::String>> View::mv_source() const {
    auto v = absorbed("mv_source", Need::Any);
    if (!v) return v.error();
    return detail::scan::mv_source(*v);
}

dstd::Result<dstd::String> View::materialize_dir() const {
    auto v = absorbed("materialize_dir", Need::Any);
    if (!v) return v.error();
    return detail::scan::materialize_dir(*v);
}

dstd::Result<void> View::register_materialized(const dstd::String& dir) const {
    auto v = absorbed("register_materialized", Need::Any);
    if (!v) return v.error();
    return detail::scan::register_materialized(*v, dir);
}

View View::select(dstd::Vector<dstd::String> names) const {
    if (detail::is_row_query(*plan_) &&
        !dataframe::detail::first_non_filter_op(lf_))
        return reshape("select", [&](const detail::scan::ScanPlan& v) {
            return detail::scan::select(v, std::move(names));
        });
    return LazyOps<View>::select(
        dstd::Vector<dstd::String>(names.begin(), names.end()));
}

View View::filter(Query q) const {
    return reshape("filter", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::filter(v, std::move(q));
    });
}

View View::filter(const FieldExpr& pred) const {
    auto query = pred.to_duql();
    if (!query) return failed(query.error());
    return filter(std::move(*query));
}

View View::phase(Phase p) const {
    return reshape("phase", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::phase(v, p);
    });
}

View View::time_range(double begin, double end) const {
    return reshape("time_range", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::time_range(v, begin, end);
    });
}

View View::time_bucket(std::uint64_t interval_us) const {
    return reshape("time_bucket", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::time_bucket(v, interval_us);
    });
}

View View::time_bucket(std::uint64_t interval_us,
                       std::uint64_t origin_us) const {
    return reshape("time_bucket", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::time_bucket(v, interval_us, origin_us);
    });
}

View View::time_bucket_min(std::uint64_t interval_us) const {
    return reshape("time_bucket_min", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::time_bucket_min(v, interval_us);
    });
}

View View::resolution(std::uint64_t cell_us) const {
    return reshape("resolution", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::occ_cell(v, cell_us);
    });
}

View View::time_scale(double ns_ratio) const {
    return reshape("time_scale", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::time_scale(v, ns_ratio);
    });
}

View View::group_by(dstd::Vector<GroupKey> keys) const {
    return reshape("group_by", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::group_by(v, std::move(keys));
    });
}

View View::agg(dstd::Vector<AggSpec> specs) const {
    return reshape("agg", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::agg(v, std::move(specs));
    });
}

View View::agg(dstd::Vector<FieldAggExpr> exprs) const {
    return reshape("agg", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::agg(v, std::move(exprs));
    });
}

View View::agg_numeric_args() const {
    return reshape("agg_numeric_args", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::agg_numeric_args(v);
    });
}

View View::agg_numeric_args(dstd::Vector<AggSpec> reductions) const {
    return reshape("agg_numeric_args", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::agg_numeric_args(v, std::move(reductions));
    });
}

View View::record_schema(dstd::String id) const {
    return reshape("record_schema", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::record_schema(v, id);
    });
}

View View::rollup_root(dstd::String dir) const {
    return reshape("rollup_root", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::rollup_root(v, std::move(dir));
    });
}

View View::views_root(dstd::String dir) const {
    return reshape("views_root", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::views_root(v, std::move(dir));
    });
}

View View::cancel_when(dstd::Function<bool()> pred) const {
    return reshape("cancel_when", [&](const detail::scan::ScanPlan& v) {
        return detail::scan::cancel_when(v, std::move(pred));
    });
}

View View::count_scans(dstd::SharedPtr<ScanCounter> counter) const {
    detail::scan::ScanPlan next =
        detail::scan::count_scans(plan_, std::move(counter));
    return View(next, dataframe::detail::rebase(
                          lf_, dstd::make_shared<ViewSource>(next)));
}

View View::memory_budget(std::uint64_t bytes) const {
    detail::scan::ScanPlan next = detail::scan::memory_budget(plan_, bytes);
    dataframe::LazyFrame lf =
        dataframe::detail::rebase(lf_, dstd::make_shared<ViewSource>(next))
            .memory_budget(bytes);
    return View(std::move(next), std::move(lf));
}

}  // namespace dftracer::utils::trace::views
