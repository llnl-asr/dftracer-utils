// DFTU_SVC_AGG: a columnar fold accumulates each batch's key + value columns
// into the dataframe engine's cross-batch AggState, merged across worker slices
// and finalized to a native dataframe result (no Arrow hop).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/plugins/fold_adapter.h>

#include <memory>
// After fold_adapter.h so nanoarrow is set up before dataframe/abi.h's
// arrow_abi.
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/agg_op_codes.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::StringIntern;
using dftracer::utils::plugins::AggCol;
using dftracer::utils::plugins::Host;
using dftracer::utils::plugins::NamedResultRegistry;
using dftracer::utils::plugins::OwnedDataFrame;
using dftracer::utils::plugins::PluginFold;
using dftracer::utils::plugins::SharedResultRegistry;
using dftracer::utils::trace::RecordPhase;
namespace agg = dftracer::utils::plugins::agg;
namespace coro = dftracer::utils::coro;
namespace views = dftracer::utils::trace::views;
using views::detail::FoldBatch;
using views::detail::FoldEvent;
using views::detail::ScanUnit;

namespace {

FoldEvent evt(StringIntern& intern, const char* cat, const char* name,
              std::uint64_t dur) {
    FoldEvent fe;
    fe.cat_id = intern.get_or_insert(cat);
    fe.name_id = intern.get_or_insert(name);
    fe.phase = RecordPhase::COMPLETE;
    fe.ts = 1;
    fe.dur = dur;
    fe.has_dur = true;
    return fe;
}

// A raw columnar plugin: on_batch builds one dft.ext.agg accumulator
// keyed by "cat" with sum(dur), p50(dur) and set_union(name), and folds each
// batch's columns into it. The host merges and finalizes it to a native
// dataframe named "by_cat".
::dftu_task* agg_on_batch(void* slice, const dftu_dataframe* df,
                          const dftu_plugin_host* host) {
    (void)slice;
    const auto* agg = static_cast<const dftu_svc_agg*>(
        host->get_service(host->h, DFTU_SVC_AGG));
    if (!agg) return nullptr;
    const dftu_agg_col specs[] = {
        {DFTU_AGG_SUM, "dur", "sum_dur", 0.0, nullptr},
        {DFTU_AGG_PCT, "dur", "p50_dur", 0.5, nullptr},
        {DFTU_AGG_SET_UNION, "name", "names", 0.0, nullptr},
    };
    const char* keys[] = {"cat"};
    dftu_agg* a = agg->agg_new(host->h, "by_cat", keys, 1, specs, 3);
    if (a) agg->agg_accumulate(host->h, a, df);
    return nullptr;
}

// Same accumulator, built through the agg:: factories + Host::agg instead of
// raw dftu_agg_col structs, proving each factory produces the same wire spec.
::dftu_task* agg_factory_on_batch(void* slice, const dftu_dataframe* df,
                                  const dftu_plugin_host* host) {
    (void)slice;
    Host h(host);
    const auto acc =
        h.agg("by_cat_factory", {"cat"},
              {agg::mean("dur", "mean_dur"), agg::pct("dur", "p50_dur", 0.5)});
    if (acc) acc.accumulate(df);
    return nullptr;
}

dftu_plugin make_agg_factory_plugin() {
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
    p.on_batch = agg_factory_on_batch;
    return p;
}

dftu_plugin make_agg_plugin() {
    dftu_plugin p{};
    p.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    p.plan_query = [](void*) -> const char* { return nullptr; };
    // This plugin holds no per-slice state (the accumulator lives in the host's
    // PluginFold), so a non-null sentinel is all PluginFold::step needs - no
    // per-slice allocation.
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
    p.on_batch = agg_on_batch;
    return p;
}

void finalize_now(PluginFold& f) {
    Runtime rt(1);
    rt.scope("fin", [&](CoroScope&) -> coro::CoroTask<void> {
          co_await f.finalize(views::detail::CoverageSet{});
      }).wait();
    rt.shutdown();
}

// One flat numeric cell as a double (Int64/UInt64/Float64 result columns).
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

// The elements of row `r` of a list<string> result column (LIST_SORTED, TOPK,
// BOTTOMK, SAMPLE): the list offsets index the flattened child column.
std::vector<std::string> list_at(const dftu_series* col, std::int64_t r) {
    std::vector<std::string> out;
    const std::int32_t* lo = dftu_series_offsets(col);
    dftu_series* vals = dftu_series_child(col, 0);
    if (!lo || !vals) {
        if (vals) dftu_series_free(vals);
        return out;
    }
    dftu_series* flat = dftu_series_materialize(vals);
    const std::int32_t* off = dftu_series_offsets(flat);
    const char* data = static_cast<const char*>(dftu_series_data(flat));
    for (std::int32_t i = lo[r]; i < lo[r + 1]; ++i)
        out.emplace_back(data + off[i],
                         static_cast<std::size_t>(off[i + 1] - off[i]));
    dftu_series_free(flat);
    dftu_series_free(vals);
    return out;
}

// Row `r` of an APPROX_TOPK result column: a list<struct{value, count}>, so the
// list child is a struct whose two fields are read side by side.
std::vector<std::pair<std::string, double>> heavy_at(const dftu_series* col,
                                                     std::int64_t r) {
    std::vector<std::pair<std::string, double>> out;
    const std::int32_t* lo = dftu_series_offsets(col);
    dftu_series* rows = dftu_series_child(col, 0);
    if (!lo || !rows) {
        if (rows) dftu_series_free(rows);
        return out;
    }
    dftu_series* value = dftu_series_child(rows, 0);
    dftu_series* count = dftu_series_child(rows, 1);
    if (value && count)
        for (std::int32_t i = lo[r]; i < lo[r + 1]; ++i)
            out.emplace_back(str_at(value, i), num_at(count, i));
    if (value) dftu_series_free(value);
    if (count) dftu_series_free(count);
    dftu_series_free(rows);
    return out;
}

// The finalized frame a plugin's accumulator was emitted under, or null.
dftu_dataframe* result_frame(NamedResultRegistry& named, const char* name) {
    auto it = named.results().find(name);
    if (it == named.results().end()) return nullptr;
    if (!std::holds_alternative<OwnedDataFrame>(it->second)) return nullptr;
    return std::get<OwnedDataFrame>(it->second).handle;
}

// Group rows come back in accumulate order, not key order, so every assertion
// looks its group up by key.
std::int64_t row_of(const dftu_series* keycol, const std::string& key) {
    for (std::int64_t r = 0; r < dftu_series_length(keycol); ++r)
        if (str_at(keycol, r) == key) return r;
    return -1;
}

// The columnar seam materializes cat/name/pid/tid/ts/dur, so every op's value
// and `by` column comes straight off the batch frame.
FoldEvent evt_full(StringIntern& intern, const char* cat, const char* name,
                   std::uint64_t pid, std::uint64_t tid, std::uint64_t ts,
                   std::uint64_t dur) {
    FoldEvent fe;
    fe.cat_id = intern.get_or_insert(cat);
    fe.name_id = intern.get_or_insert(name);
    fe.phase = RecordPhase::COMPLETE;
    fe.pid = pid;
    fe.tid = tid;
    fe.ts = ts;
    fe.dur = dur;
    fe.has_dur = true;
    return fe;
}

// Two worker slices over one dataset, so every case below also exercises the
// cross-slice merge. POSIX/pid 1: (ts, dur) = (1,10) (2,20) (3,30), an exact
// line dur = 10*ts. STDIO/pid 2: (1,5) (2,7), the line dur = 2*ts + 3.
std::vector<std::vector<FoldEvent>> sample_slices(StringIntern& intern) {
    return {
        {evt_full(intern, "POSIX", "read", 1, 100, 1, 10),
         evt_full(intern, "POSIX", "write", 1, 200, 2, 20),
         evt_full(intern, "STDIO", "open", 2, 50, 1, 5)},
        {evt_full(intern, "POSIX", "read", 1, 300, 3, 30),
         evt_full(intern, "STDIO", "close", 2, 70, 2, 7)},
    };
}

::dftu_task* no_finalize(void*, const dftu_plugin_host*) { return nullptr; }
::dftu_task* no_columns(void*, const dftu_dataframe*, const dftu_plugin_host*) {
    return nullptr;
}

dftu_plugin make_columns_plugin(
    ::dftu_task* (*on_columns)(void*, const dftu_dataframe*,
                               const dftu_plugin_host*),
    ::dftu_task* (*on_final)(void*, const dftu_plugin_host*) = no_finalize) {
    dftu_plugin p{};
    p.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    p.plan_query = [](void*) -> const char* { return nullptr; };
    p.make_slice = [](void*) -> void* {
        static int sentinel;
        return &sentinel;
    };
    p.merge = [](void*, void*) {};
    p.on_finalize = on_final;
    p.destroy_slice = [](void*) {};
    p.destroy = [](void*) {};
    p.on_batch = on_columns;
    return p;
}

void run_slices(PluginFold& master,
                const std::vector<std::vector<FoldEvent>>& slices) {
    for (const auto& evs : slices) {
        auto slice = master.slice();
        auto* pf = static_cast<PluginFold*>(slice.get());
        ScanUnit unit{};
        pf->step(FoldBatch{std::span<const FoldEvent>(evs), unit, {}});
        master.merge(*pf);
    }
}

// A keyed accumulator with two key columns and six aggregates.
::dftu_task* multi_key_columns(void* slice, const dftu_dataframe* df,
                               const dftu_plugin_host* host) {
    (void)slice;
    Host h(host);
    const auto acc =
        h.agg("by_cat_pid", {"cat", "pid"},
              {agg::count("n"), agg::sum("dur", "sum_dur"),
               agg::min("dur", "min_dur"), agg::max("dur", "max_dur"),
               agg::mean("dur", "mean_dur"), agg::count_valid("dur", "n_dur")});
    if (acc) acc.accumulate(df);
    return nullptr;
}

// Zero key columns: the scalar-handle case, one whole-scan row.
::dftu_task* scalar_columns(void* slice, const dftu_dataframe* df,
                            const dftu_plugin_host* host) {
    (void)slice;
    Host h(host);
    const auto acc =
        h.agg("whole_scan", {},
              {agg::count("n"), agg::sum("dur", "sum_dur"),
               agg::min("dur", "min_dur"), agg::max("dur", "max_dur")});
    if (acc) acc.accumulate(df);
    return nullptr;
}

// Every op appended after DFTU_AGG_COUNT_VALID, built as raw dftu_agg_col
// structs so the test binds the DFTU_AGG_* codes themselves. The two ARGMIN
// specs share the `dur` ordering column, so they must resolve to one row.
::dftu_task* appended_ops_columns(void* slice, const dftu_dataframe* df,
                                  const dftu_plugin_host* host) {
    (void)slice;
    const auto* ext = static_cast<const dftu_svc_agg*>(
        host->get_service(host->h, DFTU_SVC_AGG));
    if (!ext) return nullptr;
    const dftu_agg_col specs[] = {
        {DFTU_AGG_ARGMIN, "name", "name_at_min", 0.0, "dur"},
        {DFTU_AGG_ARGMIN, "tid", "tid_at_min", 0.0, "dur"},
        {DFTU_AGG_BIT_OR, "tid", "tid_bits", 0.0, nullptr},
        {DFTU_AGG_DISTINCT, "name", "n_names", 1024.0, nullptr},
        {DFTU_AGG_LIST_SORTED, "name", "names_by_dur", 0.0, "dur"},
        {DFTU_AGG_TOPK, "name", "top2", 2.0, "dur"},
        {DFTU_AGG_BOTTOMK, "name", "bottom2", 2.0, "dur"},
        {DFTU_AGG_APPROX_TOPK, "name", "heavy", 64.0, nullptr},
        {DFTU_AGG_SAMPLE, "name", "name_sample", 16.0, nullptr},
        {DFTU_AGG_CORR, "dur", "corr", 0.0, "ts"},
        {DFTU_AGG_COVAR_POP, "dur", "covar_pop", 0.0, "ts"},
        {DFTU_AGG_COVAR_SAMP, "dur", "covar_samp", 0.0, "ts"},
        {DFTU_AGG_REGR_SLOPE, "dur", "slope", 0.0, "ts"},
        {DFTU_AGG_REGR_INTERCEPT, "dur", "intercept", 0.0, "ts"},
        {DFTU_AGG_REGR_R2, "dur", "r2", 0.0, "ts"},
    };
    const char* keys[] = {"cat"};
    dftu_agg* a = ext->agg_new(
        host->h, "appended_ops", keys, 1, specs,
        static_cast<std::uint32_t>(sizeof(specs) / sizeof(specs[0])));
    if (a) ext->agg_accumulate(host->h, a, df);
    return nullptr;
}

// What the consumer plugin below saw when it read the producer's accumulator.
std::int64_t g_seen_rows = -1;
double g_seen_sum = -1.0;
bool g_unknown_is_null = false;

::dftu_task* read_other_agg(void* slice, const dftu_plugin_host* host) {
    (void)slice;
    Host h(host);
    const OwnedDataFrame other = h.agg_result("whole_scan");
    if (other.handle) {
        g_seen_rows = dftu_dataframe_num_rows(other.handle);
        if (dftu_series* s = dftu_dataframe_column(other.handle, "sum_dur")) {
            g_seen_sum = num_at(s, 0);
            dftu_series_free(s);
        }
    }
    g_unknown_is_null = h.agg_result("no_such_accumulator").handle == nullptr;
    return nullptr;
}

}  // namespace

TEST_CASE("DFTU_SVC_AGG: cross-batch grouped aggregation, engine-backed") {
    StringIntern intern;
    dftu_plugin p = make_agg_plugin();
    NamedResultRegistry named;
    PluginFold master(&p, intern, nullptr, &named);

    // Two slices (two workers), each one batch, merged into the master.
    std::vector<std::vector<FoldEvent>> slices = {
        {evt(intern, "POSIX", "read", 10), evt(intern, "POSIX", "write", 20),
         evt(intern, "STDIO", "open", 5)},
        {evt(intern, "POSIX", "read", 30), evt(intern, "STDIO", "close", 7)},
    };
    for (const auto& evs : slices) {
        auto slice = master.slice();
        auto* pf = static_cast<PluginFold*>(slice.get());
        ScanUnit unit{};
        pf->step(FoldBatch{std::span<const FoldEvent>(evs), unit, {}});
        master.merge(*pf);
    }
    finalize_now(master);

    auto it = named.results().find("by_cat");
    REQUIRE(it != named.results().end());
    REQUIRE(std::holds_alternative<OwnedDataFrame>(it->second));
    dftu_dataframe* out = std::get<OwnedDataFrame>(it->second).handle;
    REQUIRE(out != nullptr);
    const std::int64_t n = dftu_dataframe_num_rows(out);
    REQUIRE(n == 2);

    dftu_series* cat = dftu_dataframe_column(out, "cat");
    dftu_series* sum = dftu_dataframe_column(out, "sum_dur");
    dftu_series* p50 = dftu_dataframe_column(out, "p50_dur");
    dftu_series* names = dftu_dataframe_column(out, "names");
    REQUIRE(cat);
    REQUIRE(sum);
    REQUIRE(p50);
    REQUIRE(names);

    std::map<std::string, double> got_sum;
    std::map<std::string, double> got_p50;
    std::map<std::string, std::string> got_names;
    for (std::int64_t r = 0; r < n; ++r) {
        std::string k = str_at(cat, r);
        got_sum[k] = num_at(sum, r);
        got_p50[k] = num_at(p50, r);
        got_names[k] = str_at(names, r);
    }

    // Sums are exact across the merged slices.
    CHECK(got_sum["POSIX"] == doctest::Approx(60.0));  // 10 + 20 + 30
    CHECK(got_sum["STDIO"] == doctest::Approx(12.0));  // 5 + 7

    // Pct is a mergeable DDSketch quantile; assert it lands in range and near
    // the true median (relative accuracy, not exact).
    CHECK(got_p50["POSIX"] == doctest::Approx(20.0).epsilon(0.05));
    CHECK(got_p50["STDIO"] >= 5.0);
    CHECK(got_p50["STDIO"] <= 7.0);

    // SetUnion is the distinct value set the map path could not express.
    CHECK(got_names["POSIX"].find("read") != std::string::npos);
    CHECK(got_names["POSIX"].find("write") != std::string::npos);
    CHECK(got_names["STDIO"].find("open") != std::string::npos);
    CHECK(got_names["STDIO"].find("close") != std::string::npos);

    dftu_series_free(cat);
    dftu_series_free(sum);
    dftu_series_free(p50);
    dftu_series_free(names);

    // Parity: the same grouped sum via the dftu_series_group_by primitive,
    // over the concatenated rows.
    const char* cats[] = {"POSIX", "POSIX", "STDIO", "POSIX", "STDIO"};
    const std::uint64_t durs[] = {10, 20, 5, 30, 7};
    std::vector<std::int32_t> offs;
    std::string blob;
    offs.push_back(0);
    for (const char* c : cats) {
        blob += c;
        offs.push_back(static_cast<std::int32_t>(blob.size()));
    }
    dftu_series* keycol = dftu_series_new_string(DFTU_TYPE_STRING, offs.data(),
                                                 blob.data(), 5, nullptr);
    dftu_series* valcol =
        dftu_series_new_flat(DFTU_TYPE_UINT64, durs, 5, nullptr);
    dftu_series* gk = nullptr;
    dftu_series* gv[1] = {nullptr};
    std::int32_t nout =
        dftu_series_group_by(keycol, valcol, DFTU_REDUCE_SUM, &gk, gv, 1);
    REQUIRE(nout == 1);
    REQUIRE(gk);
    REQUIRE(gv[0]);
    std::map<std::string, double> ref_sum;
    for (std::int64_t r = 0; r < dftu_series_length(gk); ++r)
        ref_sum[str_at(gk, r)] = num_at(gv[0], r);
    CHECK(ref_sum["POSIX"] == doctest::Approx(got_sum["POSIX"]));
    CHECK(ref_sum["STDIO"] == doctest::Approx(got_sum["STDIO"]));

    dftu_series_free(keycol);
    dftu_series_free(valcol);
    dftu_series_free(gk);
    dftu_series_free(gv[0]);
}

TEST_CASE(
    "agg:: factories build the same dftu_agg_col wire fields as the raw "
    "builder") {
    const AggCol mean_c = agg::mean("dur", "mean_dur");
    const dftu_agg_col mean_raw = mean_c.raw();
    CHECK(mean_raw.op == DFTU_AGG_MEAN);
    CHECK(std::string(mean_raw.value) == "dur");
    CHECK(std::string(mean_raw.out) == "mean_dur");
    CHECK(mean_raw.by == nullptr);

    const dftu_agg_col pct_raw = agg::pct("dur", "p50_dur", 0.5).raw();
    CHECK(pct_raw.op == DFTU_AGG_PCT);
    CHECK(std::string(pct_raw.value) == "dur");
    CHECK(pct_raw.param == doctest::Approx(0.5));
    CHECK(pct_raw.by == nullptr);

    const dftu_agg_col count_raw = agg::count("n").raw();
    CHECK(count_raw.op == DFTU_AGG_COUNT);
    CHECK(count_raw.value == nullptr);
    CHECK(std::string(count_raw.out) == "n");

    const dftu_agg_col argmax_raw =
        agg::argmax("name", "name_at_max", "dur").raw();
    CHECK(argmax_raw.op == DFTU_AGG_ARGMAX);
    CHECK(std::string(argmax_raw.value) == "name");
    CHECK(std::string(argmax_raw.by) == "dur");

    const dftu_agg_col busy_raw = agg::busy("ts", "dur", "busy_us", 2.0).raw();
    CHECK(busy_raw.op == DFTU_AGG_BUSY);
    CHECK(std::string(busy_raw.value) == "ts");
    CHECK(std::string(busy_raw.by) == "dur");
    CHECK(busy_raw.param == doctest::Approx(2.0));
}

TEST_CASE(
    "agg:: factories through Host::agg accumulate the same as raw dftu_agg_col "
    "specs") {
    StringIntern intern;
    dftu_plugin p = make_agg_factory_plugin();
    NamedResultRegistry named;
    PluginFold master(&p, intern, nullptr, &named);

    std::vector<std::vector<FoldEvent>> slices = {
        {evt(intern, "POSIX", "read", 10), evt(intern, "POSIX", "write", 20),
         evt(intern, "STDIO", "open", 5)},
        {evt(intern, "POSIX", "read", 30), evt(intern, "STDIO", "close", 7)},
    };
    for (const auto& evs : slices) {
        auto slice = master.slice();
        auto* pf = static_cast<PluginFold*>(slice.get());
        ScanUnit unit{};
        pf->step(FoldBatch{std::span<const FoldEvent>(evs), unit, {}});
        master.merge(*pf);
    }
    finalize_now(master);

    auto it = named.results().find("by_cat_factory");
    REQUIRE(it != named.results().end());
    REQUIRE(std::holds_alternative<OwnedDataFrame>(it->second));
    dftu_dataframe* out = std::get<OwnedDataFrame>(it->second).handle;
    REQUIRE(out != nullptr);
    const std::int64_t n = dftu_dataframe_num_rows(out);
    REQUIRE(n == 2);

    dftu_series* cat = dftu_dataframe_column(out, "cat");
    dftu_series* mean_dur = dftu_dataframe_column(out, "mean_dur");
    dftu_series* p50_dur = dftu_dataframe_column(out, "p50_dur");
    REQUIRE(cat);
    REQUIRE(mean_dur);
    REQUIRE(p50_dur);

    std::map<std::string, double> got_mean;
    std::map<std::string, double> got_p50;
    for (std::int64_t r = 0; r < n; ++r) {
        std::string k = str_at(cat, r);
        got_mean[k] = num_at(mean_dur, r);
        got_p50[k] = num_at(p50_dur, r);
    }

    // POSIX: durs {10, 20, 30}; STDIO: durs {5, 7}.
    CHECK(got_mean["POSIX"] == doctest::Approx(20.0));
    CHECK(got_mean["STDIO"] == doctest::Approx(6.0));
    CHECK(got_p50["POSIX"] == doctest::Approx(20.0).epsilon(0.05));
    CHECK(got_p50["STDIO"] >= 5.0);
    CHECK(got_p50["STDIO"] <= 7.0);

    dftu_series_free(cat);
    dftu_series_free(mean_dur);
    dftu_series_free(p50_dur);
}

TEST_CASE("DFTU_SVC_AGG: several key columns and several aggregates") {
    StringIntern intern;
    dftu_plugin p = make_columns_plugin(multi_key_columns);
    NamedResultRegistry named;
    PluginFold master(&p, intern, nullptr, &named);
    run_slices(master, sample_slices(intern));
    finalize_now(master);

    dftu_dataframe* out = result_frame(named, "by_cat_pid");
    REQUIRE(out != nullptr);
    REQUIRE(dftu_dataframe_num_rows(out) == 2);

    dftu_series* cat = dftu_dataframe_column(out, "cat");
    dftu_series* pid = dftu_dataframe_column(out, "pid");
    dftu_series* n = dftu_dataframe_column(out, "n");
    dftu_series* sum = dftu_dataframe_column(out, "sum_dur");
    dftu_series* mn = dftu_dataframe_column(out, "min_dur");
    dftu_series* mx = dftu_dataframe_column(out, "max_dur");
    dftu_series* mean = dftu_dataframe_column(out, "mean_dur");
    dftu_series* nvalid = dftu_dataframe_column(out, "n_dur");
    REQUIRE(cat);
    REQUIRE(pid);
    REQUIRE(n);
    REQUIRE(sum);
    REQUIRE(mn);
    REQUIRE(mx);
    REQUIRE(mean);
    REQUIRE(nvalid);

    // The second key column carries its own value, not a flattened composite.
    const std::int64_t posix = row_of(cat, "POSIX");
    const std::int64_t stdio = row_of(cat, "STDIO");
    REQUIRE(posix >= 0);
    REQUIRE(stdio >= 0);
    CHECK(num_at(pid, posix) == doctest::Approx(1.0));
    CHECK(num_at(pid, stdio) == doctest::Approx(2.0));

    CHECK(num_at(n, posix) == doctest::Approx(3.0));
    CHECK(num_at(sum, posix) == doctest::Approx(60.0));
    CHECK(num_at(mn, posix) == doctest::Approx(10.0));
    CHECK(num_at(mx, posix) == doctest::Approx(30.0));
    CHECK(num_at(mean, posix) == doctest::Approx(20.0));
    CHECK(num_at(nvalid, posix) == doctest::Approx(3.0));

    CHECK(num_at(n, stdio) == doctest::Approx(2.0));
    CHECK(num_at(sum, stdio) == doctest::Approx(12.0));
    CHECK(num_at(mn, stdio) == doctest::Approx(5.0));
    CHECK(num_at(mx, stdio) == doctest::Approx(7.0));
    CHECK(num_at(mean, stdio) == doctest::Approx(6.0));
    CHECK(num_at(nvalid, stdio) == doctest::Approx(2.0));

    dftu_series_free(cat);
    dftu_series_free(pid);
    dftu_series_free(n);
    dftu_series_free(sum);
    dftu_series_free(mn);
    dftu_series_free(mx);
    dftu_series_free(mean);
    dftu_series_free(nvalid);
}

TEST_CASE("DFTU_SVC_AGG: a zero-key accumulator is one whole-scan row") {
    StringIntern intern;
    dftu_plugin p = make_columns_plugin(scalar_columns);
    NamedResultRegistry named;
    PluginFold master(&p, intern, nullptr, &named);
    run_slices(master, sample_slices(intern));
    finalize_now(master);

    dftu_dataframe* out = result_frame(named, "whole_scan");
    REQUIRE(out != nullptr);
    REQUIRE(dftu_dataframe_num_rows(out) == 1);

    dftu_series* n = dftu_dataframe_column(out, "n");
    dftu_series* sum = dftu_dataframe_column(out, "sum_dur");
    dftu_series* mn = dftu_dataframe_column(out, "min_dur");
    dftu_series* mx = dftu_dataframe_column(out, "max_dur");
    REQUIRE(n);
    REQUIRE(sum);
    REQUIRE(mn);
    REQUIRE(mx);

    // Both slices' five rows reduced to a single group.
    CHECK(num_at(n, 0) == doctest::Approx(5.0));
    CHECK(num_at(sum, 0) == doctest::Approx(72.0));
    CHECK(num_at(mn, 0) == doctest::Approx(5.0));
    CHECK(num_at(mx, 0) == doctest::Approx(30.0));

    dftu_series_free(n);
    dftu_series_free(sum);
    dftu_series_free(mn);
    dftu_series_free(mx);
}

TEST_CASE("DFTU_SVC_AGG: the appended op codes reduce across merged slices") {
    StringIntern intern;
    dftu_plugin p = make_columns_plugin(appended_ops_columns);
    NamedResultRegistry named;
    PluginFold master(&p, intern, nullptr, &named);
    run_slices(master, sample_slices(intern));
    finalize_now(master);

    dftu_dataframe* out = result_frame(named, "appended_ops");
    REQUIRE(out != nullptr);
    REQUIRE(dftu_dataframe_num_rows(out) == 2);

    dftu_series* cat = dftu_dataframe_column(out, "cat");
    REQUIRE(cat);
    const std::int64_t posix = row_of(cat, "POSIX");
    const std::int64_t stdio = row_of(cat, "STDIO");
    REQUIRE(posix >= 0);
    REQUIRE(stdio >= 0);

    dftu_series* name_at_min = dftu_dataframe_column(out, "name_at_min");
    dftu_series* tid_at_min = dftu_dataframe_column(out, "tid_at_min");
    dftu_series* bits = dftu_dataframe_column(out, "tid_bits");
    dftu_series* n_names = dftu_dataframe_column(out, "n_names");
    dftu_series* by_dur = dftu_dataframe_column(out, "names_by_dur");
    dftu_series* top2 = dftu_dataframe_column(out, "top2");
    dftu_series* bottom2 = dftu_dataframe_column(out, "bottom2");
    dftu_series* heavy = dftu_dataframe_column(out, "heavy");
    dftu_series* sample = dftu_dataframe_column(out, "name_sample");
    dftu_series* corr = dftu_dataframe_column(out, "corr");
    dftu_series* covar_pop = dftu_dataframe_column(out, "covar_pop");
    dftu_series* covar_samp = dftu_dataframe_column(out, "covar_samp");
    dftu_series* slope = dftu_dataframe_column(out, "slope");
    dftu_series* intercept = dftu_dataframe_column(out, "intercept");
    dftu_series* r2 = dftu_dataframe_column(out, "r2");
    REQUIRE(name_at_min);
    REQUIRE(tid_at_min);
    REQUIRE(bits);
    REQUIRE(n_names);
    REQUIRE(by_dur);
    REQUIRE(top2);
    REQUIRE(bottom2);
    REQUIRE(heavy);
    REQUIRE(sample);
    REQUIRE(corr);
    REQUIRE(covar_pop);
    REQUIRE(covar_samp);
    REQUIRE(slope);
    REQUIRE(intercept);
    REQUIRE(r2);

    // ARGMIN: the String repr of `value` at the row minimizing `by`.
    CHECK(str_at(name_at_min, posix) == "read");  // dur 10
    CHECK(str_at(name_at_min, stdio) == "open");  // dur 5
    // Both ARGMIN specs share the `dur` ordering column, so they report the
    // SAME row: POSIX's dur-10 row is (read, tid 100), never the other "read"
    // row at tid 300.
    CHECK(str_at(tid_at_min, posix) == "100");
    CHECK(str_at(tid_at_min, stdio) == "50");

    // BIT_OR reads `value` as u64: 100|200|300 and 50|70.
    CHECK(num_at(bits, posix) == doctest::Approx(492.0));
    CHECK(num_at(bits, stdio) == doctest::Approx(118.0));

    // DISTINCT is a KMV estimate, exact below k: {read, write} and
    // {open, close}.
    CHECK(num_at(n_names, posix) == doctest::Approx(2.0));
    CHECK(num_at(n_names, stdio) == doctest::Approx(2.0));

    // LIST_SORTED keeps every repr ordered by `by` ascending, duplicates
    // included.
    CHECK(list_at(by_dur, posix) ==
          std::vector<std::string>{"read", "write", "read"});
    CHECK(list_at(by_dur, stdio) == std::vector<std::string>{"open", "close"});

    // TOPK is descending by `by`, BOTTOMK ascending, both capped at k = 2.
    CHECK(list_at(top2, posix) == std::vector<std::string>{"read", "write"});
    CHECK(list_at(top2, stdio) == std::vector<std::string>{"close", "open"});
    CHECK(list_at(bottom2, posix) == std::vector<std::string>{"read", "write"});
    CHECK(list_at(bottom2, stdio) == std::vector<std::string>{"open", "close"});

    // APPROX_TOPK is exact while the distinct count stays under the counter
    // capacity; POSIX saw "read" twice and "write" once.
    const auto posix_heavy = heavy_at(heavy, posix);
    REQUIRE(posix_heavy.size() == 2);
    CHECK(posix_heavy[0].first == "read");
    CHECK(posix_heavy[0].second == doctest::Approx(2.0));
    CHECK(posix_heavy[1].first == "write");
    CHECK(posix_heavy[1].second == doctest::Approx(1.0));
    CHECK(heavy_at(heavy, stdio).size() == 2);

    // SAMPLE is the sorted distinct repr set once k exceeds the distinct count.
    CHECK(list_at(sample, posix) == std::vector<std::string>{"read", "write"});
    CHECK(list_at(sample, stdio) == std::vector<std::string>{"close", "open"});

    // Co-moments over (x = ts, y = dur). POSIX is dur = 10*ts, STDIO is
    // dur = 2*ts + 3, so both correlate perfectly.
    CHECK(num_at(corr, posix) == doctest::Approx(1.0));
    CHECK(num_at(covar_pop, posix) == doctest::Approx(20.0 / 3.0));
    CHECK(num_at(covar_samp, posix) == doctest::Approx(10.0));
    CHECK(num_at(slope, posix) == doctest::Approx(10.0));
    CHECK(num_at(intercept, posix) == doctest::Approx(0.0));
    CHECK(num_at(r2, posix) == doctest::Approx(1.0));

    CHECK(num_at(corr, stdio) == doctest::Approx(1.0));
    CHECK(num_at(covar_pop, stdio) == doctest::Approx(0.5));
    CHECK(num_at(covar_samp, stdio) == doctest::Approx(1.0));
    CHECK(num_at(slope, stdio) == doctest::Approx(2.0));
    CHECK(num_at(intercept, stdio) == doctest::Approx(3.0));
    CHECK(num_at(r2, stdio) == doctest::Approx(1.0));

    dftu_series_free(cat);
    dftu_series_free(name_at_min);
    dftu_series_free(tid_at_min);
    dftu_series_free(bits);
    dftu_series_free(n_names);
    dftu_series_free(by_dur);
    dftu_series_free(top2);
    dftu_series_free(bottom2);
    dftu_series_free(heavy);
    dftu_series_free(sample);
    dftu_series_free(corr);
    dftu_series_free(covar_pop);
    dftu_series_free(covar_samp);
    dftu_series_free(slope);
    dftu_series_free(intercept);
    dftu_series_free(r2);
}

TEST_CASE("DFTU_SVC_AGG: agg_result reads another plugin's merged result") {
    StringIntern intern;
    SharedResultRegistry shared;
    NamedResultRegistry named;

    dftu_plugin producer = make_columns_plugin(scalar_columns);
    dftu_plugin consumer = make_columns_plugin(no_columns, read_other_agg);
    PluginFold prod(&producer, intern, &shared, &named);
    PluginFold cons(&consumer, intern, &shared, &named);

    g_seen_rows = -1;
    g_seen_sum = -1.0;
    g_unknown_is_null = false;

    run_slices(prod, sample_slices(intern));
    // The producer must finalize first: that is when it publishes its merged
    // accumulators for a later plugin to read.
    finalize_now(prod);
    finalize_now(cons);

    CHECK(g_seen_rows == 1);
    CHECK(g_seen_sum == doctest::Approx(72.0));
    // An unknown name yields a null frame, not an empty one.
    CHECK(g_unknown_is_null);
}
