// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/plugins/host/phase_seed.h>
#include <dftracer/utils/plugins/host/port_hub.h>
#include <dftracer/utils/trace/index/cache/cache_store.h>
#include <dftracer/utils/trace/index/cache/phase_seed_store.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>

#include <algorithm>

namespace dftracer::utils::plugins::host {
namespace {

using trace::index::cache::InputStats;
using trace::views::detail::Fold;
using trace::views::detail::FoldBatch;
using trace::views::detail::ScanUnit;
using trace::views::detail::ViewPlan;

struct Found {
    InputStats stats;
    SeedSource source = SeedSource::None;
};

// Counts what the index statistics count, which is what counts_as_event
// (trace/views/fold_event.h) counts: every record with a "ph" other than
// metadata, whatever its phase letter, by its start and end. The scan delivers
// no record without a "ph".
class InputStatsFold : public Fold {
   public:
    explicit InputStatsFold(const dftracer::utils::dstd::StringIntern* intern)
        : intern_(intern) {}

    InputStats stats;
    bool any_start = false;

    bool accepts(const trace::views::detail::ScanShape&) const override {
        return true;
    }
    bool wants_metadata() const override { return false; }
    // A line with no "ph" arrives as a blank event, which a "B" or "i" record
    // also looks like by phase, so the key itself is captured.
    dstd::Vector<dstd::String> extra_captures() const override {
        return {"ph"};
    }
    dstd::UniquePtr<Fold> slice() const override {
        return dstd::make_unique<InputStatsFold>(intern_);
    }
    dstd::Result<void> step(const FoldBatch& batch) override {
        for (const auto& ev : batch.events) {
            if (ev.by_path) {
                return common::make_error<
                    "phase seeds read dftracer events, but the record schema "
                    "of these files reads records by path">(
                    common::ErrorCode::InvalidArgument);
            }
            if (ev.phase == trace::RecordPhase::Metadata || !has_ph(ev)) {
                continue;
            }
            ++stats.event_count;
            if (ev.ts > 0) {
                stats.ts_min =
                    any_start ? std::min(stats.ts_min, ev.ts) : ev.ts;
                any_start = true;
            }
            stats.ts_max = std::max(stats.ts_max, ev.ts + ev.dur);
        }
        return {};
    }
    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}
    dstd::Result<void> merge(Fold& other) override {
        const auto& o = static_cast<const InputStatsFold&>(other);
        stats.event_count += o.stats.event_count;
        stats.ts_max = std::max(stats.ts_max, o.stats.ts_max);
        if (o.any_start) {
            stats.ts_min = any_start ? std::min(stats.ts_min, o.stats.ts_min)
                                     : o.stats.ts_min;
            any_start = true;
        }
        return {};
    }
    coro::CoroTask<dstd::Result<bool>> finalize(
        const trace::views::detail::CoverageSet&) override {
        co_return true;
    }

   private:
    bool has_ph(const trace::views::detail::FoldEvent& ev) const {
        for (const auto& field : ev.top_fields) {
            if (intern_->resolve(field.first) == "ph") return true;
        }
        return false;
    }

    const dftracer::utils::dstd::StringIntern* intern_;
};

// The files of `plan` scanned whole, with no filter, phase, window or shaping
// op of the plan's view: the same events the index statistics cover. A stale
// index is rebuilt first, as a view of those files does when it is made, so the
// scan reads the bytes the fingerprint names.
coro::CoroTask<dstd::Result<InputStats>> scan_input_stats(
    trace::views::detail::ScanPlan plan) {
    dstd::SharedPtr<ViewPlan> whole;
    {
        coro::BlockingRegion blocking;
        whole = dstd::make_shared<ViewPlan>(
            *trace::views::detail::scan::from_files(plan->files));
    }
    if (whole->error) co_return *whole->error;
    whole->record_schema = plan->record_schema;
    whole->record_schema_error = plan->record_schema_error;
    whole->scan_counter = plan->scan_counter;
    whole->cancelled = plan->cancelled;
    whole->memory_budget = plan->memory_budget;
    whole->index_snapshots = plan->index_snapshots;
    const trace::views::View view = trace::views::detail::view_of(whole);
    dftracer::utils::dstd::StringIntern intern;
    InputStatsFold fold(&intern);
    Fold* folds[] = {&fold};
    auto op = view.run_folds(dstd::Span<Fold* const>(folds), intern);
    auto ran = co_await op;
    if (!ran) co_return ran.error();
    co_return fold.stats;
}

// The first path whose key in `before` is not in `after`, or empty when none.
dstd::String first_changed(
    const dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>>& before,
    const dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>>& after) {
    for (const auto& file : before) {
        bool same = false;
        for (const auto& now : after) same = same || now == file;
        if (!same) return file.first;
    }
    return after.empty() ? dstd::String() : after.front().first;
}

// The first file of `plan` that cannot be stat'ed, for the error of a file
// that went away during the pass.
dstd::String first_missing(const trace::views::detail::ViewPlan& plan) {
    for (const auto& f : plan.files) {
        common::InputFileStat now;
        if (!common::stat_input(f.file_path, now))
            return fileio::get_logical_path(f.file_path);
    }
    return {};
}

// The pass, run again once when a file changed while it read. The values are
// returned only for files that were the same before and after the scan, so
// they never mix two states of an input.
coro::CoroTask<dstd::Result<InputStats>> scan_stable_input_stats(
    trace::views::detail::ScanPlan plan, dstd::String cache_path) {
    constexpr int ATTEMPTS = 2;
    dstd::String changed;
    for (int attempt = 0; attempt < ATTEMPTS; ++attempt) {
        const auto before = trace::index::cache::input_file_keys(*plan);
        auto scan = scan_input_stats(plan);
        auto scanned = co_await scan;
        if (!scanned) co_return scanned.error();
#if DFTRACER_UTILS_TEST_PROBES
        if (g_phase_seed_pass_hook != nullptr) g_phase_seed_pass_hook(attempt);
#endif
        if (!before) co_return *scanned;
        const auto after = trace::index::cache::input_file_keys(*plan);
        if (after && *after == *before) {
            if (!cache_path.empty()) {
                const std::uint64_t key =
                    trace::index::cache::fingerprint_of_keys(*before);
                auto writer = trace::index::cache::CacheStore::open(
                    cache_path,
                    trace::index::cache::CacheStore::Mode::ReadWrite);
                const auto kept =
                    writer ? trace::index::cache::persist_input_stats(
                                 *writer, key, *scanned)
                           : dstd::Result<void>(
                                 common::make_error<
                                     "the phase seed cache could not be "
                                     "opened">(common::ErrorCode::Io));
                if (!kept) {
                    DFTRACER_UTILS_LOG_WARN(
                        "phase seed cache: not kept at {}: {}", cache_path,
                        kept.error().message());
                }
            }
            co_return *scanned;
        }
        changed = after ? first_changed(*before, *after) : first_missing(*plan);
    }
    co_return common::make_error<
        "{} changed while the host read it for the phase seeds, twice; no "
        "seed is given for a file that keeps changing">(common::ErrorCode::Io,
                                                        changed);
}

coro::CoroTask<dstd::Result<Found>> find_input_stats(
    trace::views::detail::ScanPlan plan) {
    if (auto indexed = trace::index::cache::index_input_stats(*plan))
        co_return Found{*indexed, SeedSource::Index};

    const auto fingerprint = trace::index::cache::input_fingerprint(*plan);
    const dstd::String path = trace::index::cache::phase_seed_cache_path(*plan);
    if (fingerprint && !path.empty() &&
        trace::index::cache::CacheStore::exists(path)) {
        auto db = trace::index::cache::CacheStore::open(
            path, trace::index::cache::CacheStore::Mode::ReadOnly);
        if (!db) {
            DFTRACER_UTILS_LOG_WARN("phase seed cache: cannot be opened at {}",
                                    path);
        } else {
            auto cached =
                trace::index::cache::read_input_stats(*db, *fingerprint);
            if (!cached) {
                DFTRACER_UTILS_LOG_WARN(
                    "phase seed cache: unreadable at {}: {}", path,
                    cached.error().message());
            } else if (*cached) {
                co_return Found{**cached, SeedSource::Cache};
            }
        }
    }

    auto scan = scan_stable_input_stats(plan, path);
    auto scanned = co_await scan;
    if (!scanned) co_return scanned.error();
    co_return Found{*scanned, SeedSource::Pass};
}

std::uint64_t seed_value(const InputStats& stats, std::int32_t seed) {
    switch (seed) {
        case DFTU_PHASE_SEED_TS_MIN:
            return stats.ts_min;
        case DFTU_PHASE_SEED_TS_MAX:
            return stats.ts_max;
        default:
            return stats.event_count;
    }
}

}  // namespace

coro::CoroTask<dstd::Result<SeedSource>> seed_phases(
    trace::views::detail::ScanPlan plan, dstd::Vector<PhaseDecl> phases,
    PortHub* hub) {
    const bool seeded =
        std::any_of(phases.begin(), phases.end(),
                    [](const PhaseDecl& p) { return p.seed != 0; });
    if (!seeded) co_return SeedSource::None;
    if (plan->error) co_return *plan->error;

    auto find = find_input_stats(plan);
    auto found = co_await find;
    if (!found) co_return found.error();
    for (const PhaseDecl& decl : phases) {
        if (decl.seed == 0) continue;
        PortPhase* phase = hub->phase(decl.name);
        if (phase == nullptr) continue;
        auto filled =
            phase->seed(decl.generation, seed_value(found->stats, decl.seed));
        if (!filled) co_return filled.error();
    }
    co_return found->source;
}

}  // namespace dftracer::utils::plugins::host
