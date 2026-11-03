// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/statistics/detail_stats_view.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <fstream>

using namespace dftracer::utils;

using namespace dftracer::utils::trace::internal;
using namespace dftu_utils_test;

using namespace test_view_common;
using namespace dftracer::utils::trace;

static statistics::DetailedStatistics run_detail(
    const statistics::DetailStatsView& sv,
    const statistics::DetailNeeds& needs) {
    dftracer::utils::Runtime rt;
    statistics::DetailedStatistics result;
    rt.block_on(dftracer::utils::coro::named(
        "detail", dftracer::utils::coro::with_scope(
                      [&](dftracer::utils::coro::Scope&)
                          -> dftracer::utils::coro::CoroTask<void> {
                          result = co_await sv.collect(needs);
                      })));
    return result;
}

// The DETAILED report folds each scanned event into a DetailedStatistics over a
// View's parallel scan. This checks the fold produces the same per-group and
// global duration accounting the bespoke chunk scanner did.
TEST_SUITE("detail stats via View") {
    TEST_CASE("global duration over all events") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_mixed_trace(env, 30, 20);  // 50 events
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        statistics::DetailNeeds needs;                      // no group-by
        statistics::DetailedStatistics d =
            run_detail(statistics::DetailStatsView::from_file(
                           dstd::String(gz), dstd::String(idx)),
                       needs);

        CHECK(d.events_scanned == 50);
        CHECK(d.duration.count() == 50);
        CHECK(d.grouped_duration.empty());
    }

    TEST_CASE("an event without dur adds no duration") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        const dstd::String pfw = env.get_dir() + "/instants.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            for (int i = 0; i < 5; ++i)
                ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,)"
                    << R"("tid":1,"ts":)" << (1000 + i * 100) << R"(,"dur":)"
                    << (10 + i) << R"(,"args":{}})" << "\n";
            for (int i = 0; i < 3; ++i)
                ofs << R"({"ph":"i","name":"read","cat":"POSIX","pid":1,)"
                    << R"("tid":1,"ts":)" << (2000 + i * 100)
                    << R"(,"args":{}})" << "\n";
        }
        const dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        (void)dstd::fs::remove(pfw);
        const dstd::String idx = dstd::String(determine_index_path(gz, ""));

        dstd::Vector<dstd::String> group_by{"name"};
        statistics::DetailNeeds needs;
        needs.group_by = &group_by;
        statistics::DetailedStatistics d =
            run_detail(statistics::DetailStatsView::from_file(
                           dstd::String(gz), dstd::String(idx)),
                       needs);

        CHECK(d.events_scanned == 8);
        CHECK(d.duration.count() == 5);
        CHECK(d.grouped_duration.at("read").count() == 5);
    }

    TEST_CASE("grouped by name") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_mixed_trace(env, 30, 20);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        dstd::Vector<dstd::String> group_by{"name"};
        statistics::DetailNeeds needs;
        needs.group_by = &group_by;
        statistics::DetailedStatistics d =
            run_detail(statistics::DetailStatsView::from_file(
                           dstd::String(gz), dstd::String(idx)),
                       needs);

        CHECK(d.grouped_duration.size() == 2);  // read, fwrite
        std::uint64_t total = 0;
        for (const auto& [k, v] : d.grouped_duration) total += v.count();
        CHECK(total == 50);
        CHECK(d.grouped_duration.at("read").count() == 30);
        CHECK(d.grouped_duration.at("fwrite").count() == 20);
    }

    TEST_CASE("grouped by cat with a name filter") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_mixed_trace(env, 30, 20);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        dstd::Vector<dstd::String> group_by{"cat"};
        dstd::Vector<dstd::String> filter_names{"read"};
        statistics::DetailNeeds needs;
        needs.group_by = &group_by;
        needs.filter_names = &filter_names;
        statistics::DetailedStatistics d =
            run_detail(statistics::DetailStatsView::from_file(
                           dstd::String(gz), dstd::String(idx)),
                       needs);

        CHECK(d.events_scanned == 30);  // only POSIX "read"
        CHECK(d.grouped_duration.size() == 1);
        CHECK(d.grouped_duration.at("POSIX").count() == 30);
    }
}
