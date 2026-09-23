// dftu_plugin::transform: a plugin rewrites the batch, and every plugin after
// it in fold order receives the rewrite instead of the scanned events - rows
// dropped, values changed, a column added - while a plugin before it still
// sees the scan as it was. A later plugin's reads projection and plan_query
// apply to the rewritten frame.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/fold_adapter.h>

// After fold_adapter.h so nanoarrow is set up before dataframe/abi.h's
// arrow_abi.
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/plugins_internal.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "test_plugin_common.h"

using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::plugins::build_injected_plugins;
using View = dftracer::utils::trace::views::View;
using ViewFile = dftracer::utils::trace::views::ViewFile;
using test_plugin_common::index_trace;
namespace coro = dftracer::utils::coro;

namespace {

constexpr int EVENTS = 40;  // pid 1 and 2 alternate; dur = 10 + i

void* make_empty_slice(void*) { return new int(0); }
void destroy_empty_slice(void* slice) { delete static_cast<int*>(slice); }
void no_merge(void*, void*) {}
void no_destroy(void*) {}
dftu_task* no_finalize(void*, const dftu_plugin_host*) { return nullptr; }

dftu_plugin make_bare_plugin() {
    dftu_plugin p{};
    p.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    p.make_slice = make_empty_slice;
    p.merge = no_merge;
    p.on_finalize = no_finalize;
    p.destroy_slice = destroy_empty_slice;
    p.destroy = no_destroy;
    return p;
}

// What a reading plugin saw over the whole scan.
struct Seen {
    std::atomic<std::int64_t> rows{0};
    std::atomic<std::uint64_t> dur{0};
    std::atomic<std::int64_t> twice_rows{0};
    std::atomic<std::int64_t> pid_columns{0};
    void reset() {
        rows = 0;
        dur = 0;
        twice_rows = 0;
        pid_columns = 0;
    }
};

Seen g_before, g_after, g_after_query;

void record(Seen& seen, const dftu_dataframe* df) {
    seen.rows += dftu_dataframe_num_rows(df);
    if (dftu_series* dur = dftu_dataframe_column(df, "dur")) {
        dftu_series* flat = dftu_series_materialize(dur);
        const auto* d =
            static_cast<const std::uint64_t*>(dftu_series_data(flat));
        for (std::int64_t i = 0; i < dftu_series_length(flat); ++i)
            seen.dur += d[i];
        dftu_series_free(flat);
        dftu_series_free(dur);
    }
    if (dftu_series* twice = dftu_dataframe_column(df, "twice")) {
        seen.twice_rows += dftu_series_length(twice);
        dftu_series_free(twice);
    }
    if (dftu_series* pid = dftu_dataframe_column(df, "pid")) {
        ++seen.pid_columns;
        dftu_series_free(pid);
    }
}

// ---- "before": ordered ahead of the transform; sees the scan as it is.
const char* const g_before_provides[2] = {"t.before", nullptr};
const char* const* before_provides(void*) { return g_before_provides; }
dftu_task* before_on_batch(void*, const dftu_dataframe* df,
                           const dftu_plugin_host*) {
    record(g_before, df);
    return nullptr;
}
dftu_plugin make_before() {
    dftu_plugin p = make_bare_plugin();
    p.on_batch = before_on_batch;
    p.provides = before_provides;
    return p;
}

// ---- "xform": keeps pid 1, doubles dur, adds "twice" = dur * 2.
const char* const g_xform_consumes[2] = {"t.before", nullptr};
const char* const g_xform_provides[2] = {"t.rewritten", nullptr};
const char* const* xform_consumes(void*) { return g_xform_consumes; }
const char* const* xform_provides(void*) { return g_xform_provides; }
// A transform gets the whole batch even when it declares reads.
const char* const g_xform_reads[2] = {"dur", nullptr};
const char* const* xform_reads(void*) { return g_xform_reads; }

dftu_dataframe* xform_transform(void*, const dftu_dataframe* df,
                                const dftu_plugin_host*) {
    dftu_series* pid = dftu_dataframe_column(df, "pid");
    dftu_series* dur = dftu_dataframe_column(df, "dur");
    REQUIRE(pid);  // reads declared dur only; a transform still sees pid
    REQUIRE(dur);
    dftu_series* fpid = dftu_series_materialize(pid);
    dftu_series* fdur = dftu_series_materialize(dur);
    const auto* p = static_cast<const std::uint64_t*>(dftu_series_data(fpid));
    const auto* d = static_cast<const std::uint64_t*>(dftu_series_data(fdur));
    std::vector<std::uint64_t> pids, durs, twice;
    for (std::int64_t i = 0; i < dftu_series_length(fpid); ++i) {
        if (p[i] != 1) continue;
        pids.push_back(p[i]);
        durs.push_back(d[i] * 2);
        twice.push_back(d[i] * 2);
    }
    dftu_series_free(fpid);
    dftu_series_free(fdur);
    dftu_series_free(pid);
    dftu_series_free(dur);
    const auto n = static_cast<std::int64_t>(pids.size());
    const char* names[3] = {"pid", "dur", "twice"};
    dftu_series* cols[3] = {
        dftu_series_new_flat(DFTU_TYPE_UINT64, pids.data(), n, nullptr),
        dftu_series_new_flat(DFTU_TYPE_UINT64, durs.data(), n, nullptr),
        dftu_series_new_flat(DFTU_TYPE_UINT64, twice.data(), n, nullptr)};
    return dftu_dataframe_new(names, cols, 3);
}

dftu_plugin make_xform() {
    dftu_plugin p = make_bare_plugin();
    p.transform = xform_transform;
    p.consumes = xform_consumes;
    p.provides = xform_provides;
    p.reads = xform_reads;
    return p;
}

// ---- "after": ordered after the transform; reads dur and twice only.
const char* const g_after_consumes[2] = {"t.rewritten", nullptr};
const char* const* after_consumes(void*) { return g_after_consumes; }
const char* const g_after_reads[3] = {"dur", "twice", nullptr};
const char* const* after_reads(void*) { return g_after_reads; }
dftu_task* after_on_batch(void*, const dftu_dataframe* df,
                          const dftu_plugin_host*) {
    record(g_after, df);
    return nullptr;
}
dftu_plugin make_after() {
    dftu_plugin p = make_bare_plugin();
    p.on_batch = after_on_batch;
    p.consumes = after_consumes;
    p.reads = after_reads;
    return p;
}

// ---- "after_query": after the transform, with a plan_query the host must
// re-apply to the rewritten rows (dur is doubled there, so the bound selects
// against the rewritten values, not the scanned ones).
const char* after_query_plan_query(void*) { return "dur > 60"; }
dftu_task* after_query_on_batch(void*, const dftu_dataframe* df,
                                const dftu_plugin_host*) {
    record(g_after_query, df);
    return nullptr;
}
dftu_plugin make_after_query() {
    dftu_plugin p = make_bare_plugin();
    p.on_batch = after_query_on_batch;
    p.consumes = after_consumes;
    p.plan_query = after_query_plan_query;
    return p;
}

std::string make_trace(dftu_utils_test::TestEnvironment& env) {
    std::string dir = env.get_dir() + "/t";
    fs::create_directories(dir);
    std::string pfw = dir + "/trace.pfw";
    std::ofstream ofs(pfw);
    for (int i = 0; i < EVENTS; ++i)
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":)" << (i % 2 + 1)
            << R"(,"tid":1,"ts":)" << (1000 + i * 100) << R"(,"dur":)"
            << (10 + i) << R"(,"args":{}})"
            << "\n";
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

void run_host(std::vector<dftu_plugin*> plugins, std::vector<ViewFile> files) {
    auto set = build_injected_plugins(std::move(plugins));
    REQUIRE(set.has_value());
    View view = View::from_files(std::move(files));
    Runtime rt(4);
    auto task = dftracer::utils::run_coro_scope(
        rt.executor(), [&](CoroScope&) -> coro::CoroTask<void> {
            auto run = co_await set->run(view);
            REQUIRE(run.has_value());
            co_return;
        });
    rt.submit(std::move(task), "plugin-transform").wait();
    rt.shutdown();
}

// The scan's totals: every event, and the pid-1 half with dur doubled.
std::uint64_t all_dur() {
    std::uint64_t s = 0;
    for (int i = 0; i < EVENTS; ++i) s += static_cast<std::uint64_t>(10 + i);
    return s;
}
std::uint64_t pid1_doubled_dur() {
    std::uint64_t s = 0;
    for (int i = 0; i < EVENTS; i += 2)
        s += 2 * static_cast<std::uint64_t>(10 + i);
    return s;
}
std::int64_t pid1_doubled_over(std::uint64_t bound) {
    std::int64_t n = 0;
    for (int i = 0; i < EVENTS; i += 2)
        if (2 * static_cast<std::uint64_t>(10 + i) > bound) ++n;
    return n;
}

}  // namespace

TEST_SUITE("PluginTransform") {
    TEST_CASE(
        "a later plugin reads the rewritten batch, an earlier one the scan") {
        dftu_utils_test::TestEnvironment env(EVENTS);
        ViewFile file = index_trace(make_trace(env));
        g_before.reset();
        g_after.reset();
        g_after_query.reset();

        dftu_plugin before = make_before();
        dftu_plugin xform = make_xform();
        dftu_plugin after = make_after();
        dftu_plugin after_query = make_after_query();
        // Registration order is deliberately scrambled; provides / consumes
        // settle it: before, xform, then the two readers.
        run_host({&after_query, &xform, &after, &before}, {file});

        // Before the transform: every event, as scanned.
        CHECK(g_before.rows == EVENTS);
        CHECK(g_before.dur == all_dur());
        CHECK(g_before.twice_rows == 0);

        // After it: the pid-1 half, dur doubled, the added column present,
        // and the reads projection honoured (no pid column delivered).
        CHECK(g_after.rows == EVENTS / 2);
        CHECK(g_after.dur == pid1_doubled_dur());
        CHECK(g_after.twice_rows == EVENTS / 2);
        CHECK(g_after.pid_columns == 0);

        // The plan_query ran against the rewritten values.
        CHECK(g_after_query.rows == pid1_doubled_over(60));
        CHECK(g_after_query.rows > 0);
        CHECK(g_after_query.rows < EVENTS / 2);
    }

    TEST_CASE("make_plugin wires transform in place of step") {
        struct Rewriter {
            explicit Rewriter(const dftracer::utils::plugins::Config&) {}
            dftu_dataframe* transform(const dftu_dataframe*,
                                      dftracer::utils::plugins::Host) {
                return nullptr;
            }
            void merge(Rewriter&) {}
            void finalize(dftracer::utils::plugins::Host) {}
        };
        dftu_plugin* p =
            dftracer::utils::plugins::make_plugin<Rewriter>(nullptr);
        CHECK(p->transform != nullptr);
        CHECK(p->on_batch == nullptr);
        p->destroy(p->self);
    }
}
