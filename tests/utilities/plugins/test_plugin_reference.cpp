// SPDX-License-Identifier: MIT
// Loads reference_plugin.c - a real MODULE .so that touches every dftu.svc.*
// service from C - through the real Plugins/View pipeline (dlopen, a real
// scan, a real fold), and reads back its self-report. One worker: the
// service checks race-free writes into the plugin's own status table.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/config.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <utilities/plugins/test_plugin_common.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;

#ifdef REFERENCE_PLUGIN_PATH

using View = dftracer::utils::trace::views::View;
using ViewFile = dftracer::utils::trace::views::ViewFile;

namespace {

constexpr int EVENTS = 16;

ViewFile make_trace(dftu_utils_test::TestEnvironment& env) {
    const dstd::String dir = env.dir() + "/reference_trace";
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw = dir + "/trace.pfw";
    dstd::String ofs;
    for (int i = 0; i < EVENTS; ++i)
        ofs += common::str_cat(
            R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)",
            (1000 + i * 100), R"(,"dur":)", (10 + i), R"(,"args":{}})", "\n");
    dstd::fs::write_file(dstd::PathView(pfw), ofs).discard();
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    return test_plugin_common::index_trace(gz);
}

// One worker: the plugin's checks write into a process-global status table
// that is not synchronized, since every worker would compute the same
// deterministic value for a given check.
plugins::PluginRun run_single(const plugins::Plugins& set, const View& view) {
    plugins::PluginRun out;
    Runtime rt(1);
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        auto run = co_await set.run(view);
        REQUIRE(run.has_value());
        out = std::move(*run);
        co_return;
    });
    dftu_utils_test::ok(rt.submit(std::move(task), "reference-plugin").wait());
    rt.shutdown();
    return out;
}

int32_t reference_ok(plugins::PluginRun& run) {
    auto it = run.results.results().find("reference_ok");
    REQUIRE(it != run.results.results().end());
    const auto& bytes = std::get<dstd::Vector<std::byte>>(it->second);
    REQUIRE(bytes.size() == sizeof(std::int32_t));
    std::int32_t ok = 0;
    std::memcpy(&ok, bytes.data(), sizeof(ok));
    return ok;
}

// The reference plugin's tier-2 register_state result: dur summed across
// every event by the plugin's own dur_sum_update/dur_sum_merge, finalized on
// the merged master fold.
std::uint64_t reference_dur_sum(plugins::PluginRun& run) {
    auto it = run.results.results().find("reference_plugin.dur_sum");
    REQUIRE(it != run.results.results().end());
    const auto& bytes = std::get<dstd::Vector<std::byte>>(it->second);
    REQUIRE(bytes.size() == sizeof(std::uint64_t));
    std::uint64_t sum = 0;
    std::memcpy(&sum, bytes.data(), sizeof(sum));
    return sum;
}

// Independent of the plugin: dur == 10 + i for i in [0, EVENTS), matching
// make_trace.
std::uint64_t expected_dur_sum() {
    std::uint64_t total = 0;
    for (int i = 0; i < EVENTS; ++i)
        total += static_cast<std::uint64_t>(10 + i);
    return total;
}

}  // namespace

TEST_CASE("reference_plugin: every service reports OK") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());

    auto set = plugins::Plugins::builder()
                   .add(REFERENCE_PLUGIN_PATH,
                        plugins::ConfigTree::from_json_string(
                            R"({"workdir": ")" + env.dir() + R"("})"))
                   .build();
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    REQUIRE(set.has_value());

    View view = View::from_files({make_trace(env)});
    plugins::PluginRun run = run_single(*set, view);

    const dstd::String report =
        test_plugin_common::result_text(run, "reference_report");
    INFO(report);
    CHECK(reference_ok(run) == 1);
    CHECK(report.find("FAIL") == dstd::String::npos);
    CHECK(report.find("MISSING") == dstd::String::npos);
    CHECK(reference_dur_sum(run) == expected_dur_sum());
}

TEST_CASE("reference_plugin: a forced-missing service fails loudly") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());

    auto set = plugins::Plugins::builder()
                   .add(REFERENCE_PLUGIN_PATH,
                        plugins::ConfigTree::from_json_string(
                            R"({"workdir": ")" + env.dir() +
                            R"(", "force_missing": "dftu.svc.agg@1"})"))
                   .build();
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    REQUIRE(set.has_value());

    View view = View::from_files({make_trace(env)});
    plugins::PluginRun run = run_single(*set, view);

    const dstd::String report =
        test_plugin_common::result_text(run, "reference_report");
    INFO(report);
    CHECK(reference_ok(run) == 0);
    CHECK(report.find("agg: MISSING") != dstd::String::npos);
    CHECK(report.find("agg_state: MISSING") != dstd::String::npos);
}

#else

TEST_CASE("reference_plugin: skipped (Arrow disabled at configure time)") {
    WARN("reference_plugin requires DFTRACER_UTILS_ENABLE_ARROW");
}

#endif
