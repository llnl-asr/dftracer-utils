// SPDX-License-Identifier: MIT
// The examples/plugins/two_phase_normalize.cpp plugin loaded from its shared
// library: it declares a phase result seeded with the earliest event time of
// the whole input, reads it in its first batch, and reports event times
// relative to it, across files and with a filter on the view.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/build/resolve_and_build.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <plugins/test_plugin_common.h>
#include <testing_utilities.h>

#include <cstdint>

using namespace dftracer::utils;
using trace::views::View;
using trace::views::ViewFile;

#ifndef TWO_PHASE_NORMALIZE_PLUGIN_PATH
#error "TWO_PHASE_NORMALIZE_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

dstd::String write_trace(dftu_utils_test::TestEnvironment& env,
                         const char* name,
                         const dstd::Vector<std::uint64_t>& starts) {
    const dstd::String dir = env.dir() + "/normalize";
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw = dir + "/" + name + ".pfw";
    dstd::String lines;
    for (const std::uint64_t ts : starts) {
        dstd::fmt_append<
            "{\"ph\":\"X\",\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":1,"
            "\"tid\":1,\"ts\":{},\"dur\":10{}\n">(lines, ts, R"(,"args":{}})");
    }
    dstd::fs::write_file(dstd::PathView(pfw), lines).discard();
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    return gz;
}

dstd::Vector<ViewFile> build_indexes(
    const dstd::Vector<dstd::String>& gz_files) {
    trace::index::build::ResolveAndBuildInput in;
    for (const auto& gz : gz_files) in.files.emplace_back(gz);
    in.require_checkpoints = true;
    Runtime rt(2);
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        dftu_utils_test::ok(
            co_await trace::index::build::resolve_and_build_index(
                std::move(in)));
        co_return;
    });
    dftu_utils_test::ok(rt.submit(std::move(task), "normalize-index").wait());
    rt.shutdown();
    dstd::Vector<ViewFile> files;
    for (const auto& gz : gz_files) {
        files.push_back(ViewFile{
            gz, dstd::String(trace::internal::determine_index_path(gz, ""))});
    }
    return files;
}

dstd::String span_of(const View& view) {
    auto set = plugins::host::Plugins::Builder()
                   .add(TWO_PHASE_NORMALIZE_PLUGIN_PATH)
                   .build();
    REQUIRE(set.has_value());
    auto run = test_plugin_common::run_set(*set, view, "normalize-run");
    return test_plugin_common::result_text(run, "two_phase_normalize.span");
}

}  // namespace

TEST_CASE("event times are relative to the earliest event of every file") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const dstd::Vector<ViewFile> files =
        build_indexes({write_trace(env, "late", {5000, 5400, 9100}),
                       write_trace(env, "early", {1200, 1300, 3000})});

    // The origin is 1200, the start of the other file than the one a worker
    // may read first; the last event starts at 9100.
    CHECK(span_of(View::from_files(files)) ==
          "origin=1200 first=0 last=7900 events=6");
}

TEST_CASE("the origin is of the whole input, not of the filtered view") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const dstd::Vector<ViewFile> files =
        build_indexes({write_trace(env, "one", {1200, 5000, 9100})});

    const View view = View::from_files(files).filter(
        duql::Query::from_string("ts > 4000").value());
    CHECK(span_of(view) == "origin=1200 first=3800 last=7900 events=2");
}

TEST_CASE("a file with no index gets the same origin") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const dstd::String gz = write_trace(env, "plain", {700, 800, 900});
    CHECK(span_of(View::from_file(gz)) ==
          "origin=700 first=0 last=200 events=3");
}
