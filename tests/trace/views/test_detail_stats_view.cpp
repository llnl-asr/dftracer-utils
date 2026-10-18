#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/statistics/detail_stats_view.h>
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "test_view_common.h"

using namespace test_view_common;
namespace stats = dftracer::utils::trace::statistics;

static stats::DetailedStatistics run_detail(const stats::DetailStatsView& sv,
                                            const stats::DetailNeeds& needs) {
    dftracer::utils::Runtime rt;
    stats::DetailedStatistics result;
    rt.run_blocking("detail",
                    [&](dftracer::utils::CoroScope&)
                        -> dftracer::utils::coro::CoroTask<void> {
                        result = co_await sv.collect(needs);
                    });
    return result;
}

// The DETAILED report folds each scanned event into a DetailedStatistics over a
// View's parallel scan. This checks the fold produces the same per-group and
// global duration accounting the bespoke chunk scanner did.
TEST_SUITE("detail stats via View") {
    TEST_CASE("global duration over all events") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 30, 20);  // 50 events
        std::string idx = determine_index_path(gz, "");

        stats::DetailNeeds needs;                          // no group-by
        stats::DetailedStatistics d =
            run_detail(stats::DetailStatsView::from_file(gz, idx), needs);

        CHECK(d.events_scanned == 50);
        CHECK(d.duration.count() == 50);
        CHECK(d.grouped_duration.empty());
    }

    TEST_CASE("grouped by name") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 30, 20);
        std::string idx = determine_index_path(gz, "");

        std::vector<std::string> group_by{"name"};
        stats::DetailNeeds needs;
        needs.group_by = &group_by;
        stats::DetailedStatistics d =
            run_detail(stats::DetailStatsView::from_file(gz, idx), needs);

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
        std::string gz = create_mixed_trace(env, 30, 20);
        std::string idx = determine_index_path(gz, "");

        std::vector<std::string> group_by{"cat"};
        std::vector<std::string> filter_names{"read"};
        stats::DetailNeeds needs;
        needs.group_by = &group_by;
        needs.filter_names = &filter_names;
        stats::DetailedStatistics d =
            run_detail(stats::DetailStatsView::from_file(gz, idx), needs);

        CHECK(d.events_scanned == 30);  // only POSIX "read"
        CHECK(d.grouped_duration.size() == 1);
        CHECK(d.grouped_duration.at("POSIX").count() == 30);
    }
}
