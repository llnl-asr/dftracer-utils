#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/bloom_fold.h>
#include <dftracer/utils/trace/views/coverage.h>
#include <dftracer/utils/trace/views/dict_fold.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view_planner_utility.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

namespace dftracer::utils::trace::views::detail {

std::size_t checkpoint_size_or_default(std::size_t s) {
    return s != 0
               ? s
               : utilities::indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE;
}

std::optional<query::Query> effective_query(const ViewPlan& plan) {
    RecordPhase rp;
    if (plan.phase == Phase::Events) {
        rp = RecordPhase::COMPLETE;
    } else if (plan.phase == Phase::Counters) {
        rp = RecordPhase::COUNTER;
    } else if (plan.phase == Phase::Aggregated) {
        rp = RecordPhase::AGGREGATED;
    } else if (plan.phase == Phase::Metadata) {
        rp = RecordPhase::METADATA;
    } else {
        return plan.query;
    }
    // Match both the current integer "ph" and the legacy single-letter form so
    // the same View reads new- and old-format traces alike.
    std::string q = "(ph == " + std::to_string(phase_to_int(rp)) +
                    " or ph == \"" + std::string(1, phase_to_letter(rp)) +
                    "\")";
    if (plan.query) q = "(" + plan.query->source() + ") and " + q;
    return query::parse_or_throw(q);
}

bool is_cancelled(const ViewPlan& plan) {
    return plan.cancelled && plan.cancelled();
}

ViewDefinition make_vdef(const ViewPlan& plan, bool for_aggregation) {
    ViewDefinition vdef;
    vdef.query = effective_query(plan);
    if (for_aggregation) {
        // Aggregation folds parsed events; ph="M" metadata records carry no
        // aggregable fields, so drop them - unless a rank group key needs the
        // PR records to build its pid -> rank map in the same scan, or the plan
        // explicitly selects phase("metadata") to aggregate the records.
        const bool wants_rank = std::any_of(
            plan.group_by.begin(), plan.group_by.end(),
            [](const GroupKey& g) { return g.kind == GroupKey::Kind::Rank; });
        const bool wants_metadata = plan.phase == Phase::Metadata;
        vdef.include_metadata = wants_rank || wants_metadata;
        vdef.emit_all_metadata = wants_rank || wants_metadata;
        // Never filter metadata during a rank harvest: its PR records must
        // survive the event query to build the pid -> rank map.
        vdef.filter_metadata = wants_metadata && !wants_rank;
    } else {
        // phase("metadata") reads the metadata records as rows: emit every one
        // (hash records included) and apply the query to them.
        const bool wants_metadata = plan.phase == Phase::Metadata;
        vdef.include_metadata = plan.include_metadata;
        vdef.emit_all_metadata = plan.emit_all_metadata || wants_metadata;
        vdef.filter_metadata = wants_metadata;
    }
    return vdef;
}

ViewScannerInput make_scanner_input(const ScanUnit& u,
                                    const ViewDefinition& vdef,
                                    const std::optional<query::Query>& q) {
    ViewScannerInput sin;
    sin.with_file_path(u.file_path)
        .with_index_path(u.index_path)
        .with_checkpoint_size(u.checkpoint_size)
        .with_byte_range(u.start_byte, u.end_byte)
        .with_checkpoint_idx(u.checkpoint_idx)
        .with_view(vdef);
    sin.query = q;
    return sin;
}

coro::CoroTask<std::vector<ScanUnit>> gather_units(const ViewPlan& plan,
                                                   const ViewDefinition& vdef,
                                                   std::uint64_t& skipped_out) {
    std::vector<std::vector<ScanUnit>> per_file(plan.files.size());
    std::vector<std::uint64_t> skip_v(plan.files.size(), 0);

    co_await run_coro_scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        for (std::size_t fi = 0; fi < plan.files.size(); ++fi) {
            scope.spawn([&, fi](CoroScope&) -> coro::CoroTask<void> {
                if (is_cancelled(plan)) co_return;
                const auto& f = plan.files[fi];
                const std::size_t ckpt =
                    checkpoint_size_or_default(f.checkpoint_size);
                std::uint64_t uc_size = f.uncompressed_size;
                std::size_t n_ckpts = f.num_checkpoints;
                const std::string& index_path = f.index_path;

                if (uc_size == 0 || n_ckpts == 0) {
                    trace::MetadataCollectorUtility meta;
                    auto md = co_await meta(
                        trace::MetadataCollectorUtilityInput::from_file(
                            f.file_path)
                            .with_checkpoint_size(ckpt)
                            .with_force_rebuild(false)
                            .with_index(index_path));
                    // Fail loudly: dropping the file would silently omit its
                    // events from the result.
                    if (!md.success)
                        throw DFTUtilsException(
                            ErrorCode::IO,
                            "gather_units: metadata read failed for " +
                                f.file_path);
                    uc_size = md.uncompressed_size;
                    n_ckpts = md.num_checkpoints;
                }

                ViewPlannerInput pin;
                pin.with_view(vdef)
                    .with_file_path(f.file_path)
                    .with_index_path(fs::exists(index_path) ? index_path : "")
                    .with_uncompressed_size(uc_size)
                    .with_num_checkpoints(n_ckpts);
                if (plan.time_range)
                    pin.with_time_range(plan.time_range->first,
                                        plan.time_range->second);

                ViewPlannerUtility planner;
                auto planned = co_await planner(pin);
                // A planner error must propagate; file_may_match == false is a
                // legitimate prune, so that one stays a quiet skip.
                if (!planned) throw DFTUtilsException(planned.error());
                skip_v[fi] = planned->skipped_checkpoints;
                if (!planned->file_may_match) co_return;

                auto& out = per_file[fi];
                for (const auto& c : planned->candidates)
                    out.push_back(ScanUnit{f.file_path, index_path, ckpt,
                                           c.checkpoint_idx, c.start_byte,
                                           c.end_byte});
            });
        }
        co_return;
    });

    skipped_out = 0;
    for (auto s : skip_v) skipped_out += s;
    std::vector<ScanUnit> units;
    for (auto& v : per_file)
        for (auto& u : v) units.push_back(std::move(u));
    co_return units;
}

ScanShape scan_shape(const ViewDefinition& vdef) {
    ScanShape s;
    // A phase selector is folded into the query, so a ph-filtered scan reports
    // itself filtered without any user predicate - which is what artifacts
    // covering other phases need to see.
    s.filtered = vdef.query.has_value();
    s.include_metadata = vdef.include_metadata;
    s.emit_all_metadata = vdef.emit_all_metadata;
    return s;
}

namespace {

// True if any file's index still lacks the pruner bloom (or has no index yet).
// Bloom presence means the index was fully built, so once every file has it the
// ride-along stops attaching and a repeat scan pays no re-parse.
bool any_file_missing_bloom(const ViewPlan& plan) {
    namespace idx = utilities::indexer;
    for (const auto& f : plan.files) {
        try {
            idx::IndexDatabase db(f.index_path, idx::IndexOpenMode::ReadOnly);
            int fid = db.get_file_info_id(
                idx::internal::get_logical_path(f.file_path));
            if (fid < 0 || !db.has_bloom_data(fid)) return true;
        } catch (const std::exception&) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<std::unique_ptr<Fold>> select_index_folds(
    const ViewPlan& plan, const ViewDefinition& vdef,
    dftracer::utils::StringIntern& intern) {
    std::vector<std::unique_ptr<Fold>> kept;
    if (plan.files.empty() || !any_file_missing_bloom(plan)) return kept;

    const ScanShape shape = scan_shape(vdef);
    auto dict = std::make_unique<DictFold>(intern);
    if (dict->accepts(shape)) kept.push_back(std::move(dict));
    auto bloom = std::make_unique<BloomFold>(intern);
    if (bloom->accepts(shape)) kept.push_back(std::move(bloom));
    return kept;
}

coro::CoroTask<ExportStats> for_each_scanned_batch(
    const ViewPlan& plan, const ViewDefinition& vdef, std::size_t num_slots,
    std::uint64_t limit,
    const std::function<void(std::size_t,
                             const std::vector<std::string_view>&)>& on_batch,
    std::span<Fold* const> folds, dftracer::utils::StringIntern* intern) {
    std::uint64_t skipped = 0;
    auto units = co_await gather_units(plan, vdef, skipped);
    // 0 means one worker per unit (max parallelism); otherwise cap the fan-out.
    if (num_slots == 0) num_slots = std::max<std::size_t>(1, units.size());

    const std::uint64_t cap =
        limit > 0 ? limit : std::numeric_limits<std::uint64_t>::max();
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::size_t> next_unit{0};
    std::vector<std::uint64_t> matched_v(num_slots, 0);
    std::vector<std::uint64_t> scanned_v(num_slots, 0);
    const std::size_t nworkers = std::min(num_slots, units.size());
    std::vector<CoverageSet> covered_v(nworkers);

    bool any_needs_args = false;
    for (auto* f : folds) any_needs_args |= f->needs_args();
    // One slice of every fold per worker, owned here so a slice outlives the
    // coroutine that filled it; merged after the join.
    std::vector<std::vector<std::unique_ptr<Fold>>> fslice(nworkers);
    if (!folds.empty())
        for (std::size_t w = 0; w < nworkers; ++w)
            for (auto* f : folds) fslice[w].push_back(f->slice());

    co_await run_coro_scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        for (std::size_t w = 0; w < nworkers; ++w) {
            // Worker `w` hands batches to slot `w`; a coroutine never runs
            // concurrently with itself, so per-slot state needs no lock.
            scope.spawn([&, w](CoroScope&) -> coro::CoroTask<void> {
                // Sealed per unit, so a cap or cancel cutting the current unit
                // short drops only that one. Draining the batch loop is the
                // sole path to a claim; every break leaves the unit pending.
                PendingCoverage pending(covered_v[w]);
                auto& slices = fslice[w];
                simdjson::dom::parser parser;
                std::string parse_buf;
                std::vector<FoldEvent> fold_events;
                for (;;) {
                    if (produced.load(std::memory_order_relaxed) >= cap) break;
                    if (is_cancelled(plan)) break;
                    std::size_t i =
                        next_unit.fetch_add(1, std::memory_order_relaxed);
                    if (i >= units.size()) break;
                    ViewScannerInput sin =
                        make_scanner_input(units[i], vdef, vdef.query);
                    ViewScannerUtility scanner;
                    auto gen = scanner(sin);
                    bool complete = true;
                    while (auto b = co_await gen.next()) {
                        if (b->events.empty()) continue;
                        if (produced.load(std::memory_order_relaxed) >= cap) {
                            complete = false;
                            break;
                        }
                        if (is_cancelled(plan)) {
                            complete = false;
                            break;
                        }
                        matched_v[w] += b->events_matched;
                        scanned_v[w] += b->events_scanned;
                        on_batch(w, b->events);
                        if (!slices.empty()) {
                            // The scan buffer carries no simdjson padding, so
                            // parse each event into a reused padded buffer;
                            // owned FoldEvents let the folds outlive it.
                            fold_events.clear();
                            for (auto line : b->events) {
                                parse_buf.assign(line);
                                parse_buf.resize(
                                    line.size() + simdjson::SIMDJSON_PADDING,
                                    '\0');
                                auto doc = parser.parse(parse_buf.data(),
                                                        line.size(), false);
                                if (doc.error()) continue;
                                fold_events.push_back(extract_fold_event(
                                    doc.value_unsafe(), *intern,
                                    any_needs_args));
                            }
                            FoldBatch fb{
                                std::span<const FoldEvent>(fold_events),
                                units[i]};
                            for (auto& s : slices) s->step(fb);
                        }
                        produced.fetch_add(b->events.size(),
                                           std::memory_order_relaxed);
                    }
                    if (!complete) {
                        for (auto& s : slices) s->drop_unit(units[i]);
                        break;
                    }
                    for (auto& s : slices) s->seal_unit(units[i]);
                    pending.mark_complete(units[i].file_path,
                                          units[i].checkpoint_idx);
                    pending.seal();
                }
                co_return;
            });
        }
        co_return;
    });

    CoverageSet covered;
    for (auto& c : covered_v) covered.absorb(std::move(c));
    // Captured before whole-file coverage is added below, which would inflate
    // the count.
    const std::size_t member_units = covered.size();

    // A file is whole only if nothing was pruned away and every one of its
    // units sealed. Pruning is decided inside gather_units, after the folds
    // were attached, so it cannot be a requirement they declare - it has to
    // land here, as a coverage fact.
    if (skipped == 0) {
        std::unordered_map<std::string_view,
                           std::pair<std::size_t, std::size_t>>
            per_file;
        for (const auto& u : units) {
            auto& c = per_file[u.file_path];
            ++c.first;
            if (covered.covers(u.file_path, u.checkpoint_idx)) ++c.second;
        }
        for (const auto& [file, c] : per_file)
            if (c.first == c.second) covered.add_file(file);
    }

    for (std::size_t w = 0; w < nworkers; ++w)
        for (std::size_t k = 0; k < folds.size(); ++k)
            folds[k]->merge(*fslice[w][k]);

    ExportStats st;
    st.chunks_skipped = skipped;
    st.chunks_scanned = units.size();
    st.chunks_covered = member_units;
    for (auto m : matched_v) st.events_matched += m;
    for (auto s : scanned_v) st.events_scanned += s;
    st.truncated = produced.load(std::memory_order_relaxed) >= cap;
    for (auto* f : folds)
        if (co_await f->finalize(covered)) st.artifacts_committed = true;
    co_return st;
}

}  // namespace dftracer::utils::trace::views::detail
