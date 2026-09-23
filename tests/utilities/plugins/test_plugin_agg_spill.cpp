// A plugin accumulator whose group cardinality dwarfs the scan's memory budget
// must spill to disk instead of growing an unbounded group map, and must come
// back with exactly the aggregate an unbudgeted run produces.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/plugins/fold_adapter.h>

#include <memory>
// After fold_adapter.h so nanoarrow is set up before dataframe/abi.h's
// arrow_abi.
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <variant>
#include <vector>

using dftracer::utils::CoroScope;
using dftracer::utils::NO_SPILL_BUDGET;
using dftracer::utils::Runtime;
using dftracer::utils::StringIntern;
using dftracer::utils::plugins::Host;
using dftracer::utils::plugins::NamedResultRegistry;
using dftracer::utils::plugins::OwnedDataFrame;
using dftracer::utils::plugins::PluginFold;
using dftracer::utils::trace::RecordPhase;
namespace agg = dftracer::utils::plugins::agg;
namespace coro = dftracer::utils::coro;
namespace views = dftracer::utils::trace::views;
using views::detail::FoldBatch;
using views::detail::FoldEvent;
using views::detail::ScanUnit;

namespace {

constexpr const char* ACC_NAME = "by_name";
// Above the drain's per-chunk group bound, so the flat case also
// exercises the chunked finalize + concat, not just one final merge.
constexpr int GROUPS = 5000;
constexpr int BATCH = 250;
// Small enough that a few hundred groups blow past it, so the spill engages
// several times over the batches below.
constexpr std::uint64_t TINY_BUDGET = 4096;

// One key column (the per-event name, so the key cardinality is the event
// count) and a spread of ops: FieldStat-derived, sketch-derived, and a
// String-repr op, all of which have to survive a serialize/merge round trip.
::dftu_task* spill_columns(void* slice, const dftu_dataframe* df,
                           const dftu_plugin_host* host) {
    (void)slice;
    Host h(host);
    const auto acc =
        h.agg(ACC_NAME, {"name"},
              {agg::count("n"), agg::sum("dur", "sum_dur"),
               agg::mean("dur", "mean_dur"), agg::pct("dur", "p50_dur", 0.5),
               agg::argmax("cat", "cat_at_max", "dur")});
    if (acc) acc.accumulate(df);
    return nullptr;
}

// A nested-output op (list<string>) alongside a flat one: drain cannot
// vertically concat a List column, so this exercises the single-finalize path
// through the same spilled runs.
::dftu_task* spill_nested_columns(void* slice, const dftu_dataframe* df,
                                  const dftu_plugin_host* host) {
    (void)slice;
    const auto* ext = static_cast<const dftu_svc_agg*>(
        host->get_service(host->h, DFTU_SVC_AGG));
    if (!ext) return nullptr;
    const dftu_agg_col specs[] = {
        {DFTU_AGG_SUM, "dur", "sum_dur", 0.0, nullptr},
        {DFTU_AGG_TOPK, "cat", "top_cat", 2.0, "dur"},
    };
    const char* keys[] = {"name"};
    dftu_agg* a = ext->agg_new(host->h, ACC_NAME, keys, 1, specs, 2);
    if (a) ext->agg_accumulate(host->h, a, df);
    return nullptr;
}

dftu_plugin make_plugin(::dftu_task* (*on_columns)(void*, const dftu_dataframe*,
                                                   const dftu_plugin_host*)) {
    dftu_plugin p{};
    p.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    p.plan_query = [](void*) -> const char* { return nullptr; };
    p.make_slice = [](void*) -> void* {
        static int sentinel;
        return &sentinel;
    };
    p.merge = [](void*, void*) {};
    p.on_finalize = [](void*, const dftu_plugin_host*) -> ::dftu_task* {
        return nullptr;
    };
    p.destroy_slice = [](void*) {};
    p.destroy = [](void*) {};
    p.on_batch = on_columns;
    return p;
}

FoldEvent evt(StringIntern& intern, const std::string& cat,
              const std::string& name, std::uint64_t dur) {
    FoldEvent fe;
    fe.cat_id = intern.get_or_insert(cat);
    fe.name_id = intern.get_or_insert(name);
    fe.phase = RecordPhase::COMPLETE;
    fe.pid = 1;
    fe.tid = 1;
    fe.ts = dur;
    fe.dur = dur;
    fe.has_dur = true;
    return fe;
}

// GROUPS distinct keys, each seen twice (in two different slices) so every
// group's aggregate depends on the cross-slice merge as well as on the spill.
std::vector<std::vector<FoldEvent>> wide_slices(StringIntern& intern) {
    std::vector<FoldEvent> a, b;
    a.reserve(GROUPS);
    b.reserve(GROUPS);
    for (int i = 0; i < GROUPS; ++i) {
        const std::string name = "op_" + std::to_string(i);
        a.push_back(
            evt(intern, "POSIX", name, static_cast<std::uint64_t>(i + 1)));
        b.push_back(evt(intern, "STDIO", name,
                        static_cast<std::uint64_t>(2 * (i + 1))));
    }
    return {std::move(a), std::move(b)};
}

void finalize_now(PluginFold& f) {
    Runtime rt(1);
    rt.scope("fin", [&](CoroScope&) -> coro::CoroTask<void> {
          co_await f.finalize(views::detail::CoverageSet{});
      }).wait();
    rt.shutdown();
}

double num_at(const dftu_series* col, std::int64_t r) {
    dftu_series* flat = dftu_series_materialize(col);
    const void* d = dftu_series_data(flat);
    double out = 0.0;
    switch (dftu_series_type(flat)) {
        case DFTU_TYPE_INT64:
            out = static_cast<double>(static_cast<const std::int64_t*>(d)[r]);
            break;
        case DFTU_TYPE_UINT64:
            out = static_cast<double>(static_cast<const std::uint64_t*>(d)[r]);
            break;
        case DFTU_TYPE_FLOAT64:
            out = static_cast<const double*>(d)[r];
            break;
        default:
            break;
    }
    dftu_series_free(flat);
    return out;
}

std::string str_at(const dftu_series* col, std::int64_t r) {
    dftu_series* flat = dftu_series_materialize(col);
    const std::int32_t* off = dftu_series_offsets(flat);
    const char* data = static_cast<const char*>(dftu_series_data(flat));
    std::string out(data + off[r],
                    static_cast<std::size_t>(off[r + 1] - off[r]));
    dftu_series_free(flat);
    return out;
}

std::vector<std::string> list_at(const dftu_series* col, std::int64_t r) {
    std::vector<std::string> out;
    const std::int32_t* lo = dftu_series_offsets(col);
    dftu_series* vals = dftu_series_child(col, 0);
    if (!lo || !vals) {
        if (vals) dftu_series_free(vals);
        return out;
    }
    for (std::int32_t i = lo[r]; i < lo[r + 1]; ++i)
        out.push_back(str_at(vals, i));
    dftu_series_free(vals);
    return out;
}

// One row of the finalized accumulator, keyed by the group so the two runs
// compare regardless of row order (spill emits key-sorted rows, the in-memory
// path emits accumulate-order rows).
struct Row {
    double n = 0;
    double sum = 0;
    double mean = 0;
    double p50 = 0;
    std::string cat_at_max;
    std::vector<std::string> top_cat;
};

dftu_dataframe* result_frame(NamedResultRegistry& named) {
    auto it = named.results().find(ACC_NAME);
    if (it == named.results().end()) return nullptr;
    if (!std::holds_alternative<OwnedDataFrame>(it->second)) return nullptr;
    return std::get<OwnedDataFrame>(it->second).handle;
}

// Run the plugin over `wide_slices` with `budget`, returning the finalized
// rows by key and, through `runs`, the number of spill runs the master's
// accumulator wrote.
std::map<std::string, Row> run_with_budget(dftu_plugin& p, std::uint64_t budget,
                                           std::size_t* runs, bool nested) {
    StringIntern intern;
    NamedResultRegistry named;
    PluginFold master(&p, intern, nullptr, &named, "spill_test", budget);
    for (const std::vector<FoldEvent>& evs : wide_slices(intern)) {
        auto slice = master.slice();
        auto* pf = static_cast<PluginFold*>(slice.get());
        ScanUnit unit{};
        for (std::size_t at = 0; at < evs.size(); at += BATCH) {
            const std::size_t n = std::min<std::size_t>(BATCH, evs.size() - at);
            pf->step(FoldBatch{
                std::span<const FoldEvent>(evs.data() + at, n), unit, {}});
        }
        master.merge(*pf);
    }
    *runs = master.agg_spill_runs(ACC_NAME);
    finalize_now(master);

    std::map<std::string, Row> out;
    dftu_dataframe* frame = result_frame(named);
    REQUIRE(frame != nullptr);
    dftu_series* key = dftu_dataframe_column(frame, "name");
    REQUIRE(key != nullptr);
    dftu_series* sum = dftu_dataframe_column(frame, "sum_dur");
    REQUIRE(sum != nullptr);
    dftu_series* n = nested ? nullptr : dftu_dataframe_column(frame, "n");
    dftu_series* mean =
        nested ? nullptr : dftu_dataframe_column(frame, "mean_dur");
    dftu_series* p50 =
        nested ? nullptr : dftu_dataframe_column(frame, "p50_dur");
    dftu_series* cat =
        nested ? nullptr : dftu_dataframe_column(frame, "cat_at_max");
    dftu_series* top =
        nested ? dftu_dataframe_column(frame, "top_cat") : nullptr;
    for (std::int64_t r = 0; r < dftu_series_length(key); ++r) {
        Row row;
        row.sum = num_at(sum, r);
        if (!nested) {
            row.n = num_at(n, r);
            row.mean = num_at(mean, r);
            row.p50 = num_at(p50, r);
            row.cat_at_max = str_at(cat, r);
        } else {
            row.top_cat = list_at(top, r);
        }
        out.emplace(str_at(key, r), std::move(row));
    }
    dftu_series_free(key);
    dftu_series_free(sum);
    if (n) dftu_series_free(n);
    if (mean) dftu_series_free(mean);
    if (p50) dftu_series_free(p50);
    if (cat) dftu_series_free(cat);
    if (top) dftu_series_free(top);
    return out;
}

}  // namespace

TEST_CASE("plugin accumulator spills past the memory budget, same result") {
    dftu_plugin p = make_plugin(spill_columns);

    std::size_t unbounded_runs = 0;
    const std::map<std::string, Row> want =
        run_with_budget(p, NO_SPILL_BUDGET, &unbounded_runs, false);
    CHECK(unbounded_runs == 0);
    REQUIRE(want.size() == static_cast<std::size_t>(GROUPS));

    std::size_t spill_runs = 0;
    const std::map<std::string, Row> got =
        run_with_budget(p, TINY_BUDGET, &spill_runs, false);

    // The mechanism, not just the answer: a budget that is silently ignored
    // would still produce the right numbers.
    CHECK(spill_runs > 0);

    REQUIRE(got.size() == want.size());
    for (const auto& [key, exp] : want) {
        auto it = got.find(key);
        REQUIRE_MESSAGE(it != got.end(), key);
        CHECK(it->second.n == doctest::Approx(exp.n));
        CHECK(it->second.sum == doctest::Approx(exp.sum));
        CHECK(it->second.mean == doctest::Approx(exp.mean));
        CHECK(it->second.p50 == doctest::Approx(exp.p50));
        CHECK(it->second.cat_at_max == exp.cat_at_max);
    }
}

TEST_CASE("plugin accumulator spill keeps a nested (list) output column") {
    dftu_plugin p = make_plugin(spill_nested_columns);

    std::size_t unbounded_runs = 0;
    const std::map<std::string, Row> want =
        run_with_budget(p, NO_SPILL_BUDGET, &unbounded_runs, true);
    CHECK(unbounded_runs == 0);

    std::size_t spill_runs = 0;
    const std::map<std::string, Row> got =
        run_with_budget(p, TINY_BUDGET, &spill_runs, true);
    CHECK(spill_runs > 0);

    REQUIRE(got.size() == want.size());
    for (const auto& [key, exp] : want) {
        auto it = got.find(key);
        REQUIRE_MESSAGE(it != got.end(), key);
        CHECK(it->second.sum == doctest::Approx(exp.sum));
        CHECK(it->second.top_cat == exp.top_cat);
    }
}
