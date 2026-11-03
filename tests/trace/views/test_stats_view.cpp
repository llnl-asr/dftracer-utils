// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/statistics/stats_view.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <fstream>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;

using namespace test_view_common;
using namespace dftracer::utils::trace;

// Run a StatsView collection on a Runtime (StatsView is async - no blocking).
static statistics::ViewStats run_stats(const statistics::StatsView& sv,
                                       statistics::StatNeeds needs = {}) {
    dftracer::utils::Runtime rt;
    statistics::ViewStats result;
    rt.block_on(coro::named(
        "stats",
        coro::with_scope(
            [&](coro::Scope&) -> dftracer::utils::coro::CoroTask<void> {
                result = co_await sv.collect(needs);
            })));
    return result;
}

namespace {

std::size_t col(const dataframe::DataFrame& b, const dstd::String& name) {
    for (std::size_t i = 0; i < b.names.size(); ++i)
        if (b.names[i] == name) return i;
    throw std::runtime_error(("no column: " + name).c_str());
}

// Sum an integer count column regardless of its stored width.
std::int64_t sum_i64(const dataframe::DataFrame& b, const dstd::String& name) {
    const dataframe::Series& c = b.columns[col(b, name)];
    std::int64_t s = 0;
    const std::int64_t* p = c.data<std::int64_t>();
    for (std::int64_t i = 0; i < b.num_rows(); ++i) s += p[i];
    return s;
}

}  // namespace

// The stats dftracer_stats reports (category/name/pid_tid counts, duration
// stats, time range) are all View group-by aggregates. This proves View
// produces them, so the bespoke stats readers can migrate onto StatsView.
TEST_SUITE("stats via View") {
    TEST_CASE("category counts + total events") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz =
            create_mixed_trace(env, 30, 20);  // posix=30, stdio=20
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        dataframe::DataFrame cats =
            View::from_file(dstd::String(gz), dstd::String(idx))
                .group_by({GroupKey::cat()})
                .agg({{AggOp::Count, "", "n"}})
                .collect()
                .get();

        // num_categories == distinct groups; total events == sum of counts.
        CHECK(cats.num_rows() == 2);      // posix, stdio
        CHECK(sum_i64(cats, "n") == 50);  // 30 + 20
    }

    TEST_CASE("name and pid/tid group counts") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_mixed_trace(env, 30, 20);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        dataframe::DataFrame names =
            View::from_file(dstd::String(gz), dstd::String(idx))
                .group_by({GroupKey::name()})
                .agg({{AggOp::Count, "", "n"}})
                .collect()
                .get();
        CHECK(names.num_rows() > 0);  // num_unique_names
        CHECK(sum_i64(names, "n") == 50);

        dataframe::DataFrame pt =
            View::from_file(dstd::String(gz), dstd::String(idx))
                .group_by({GroupKey::pid(), GroupKey::tid()})
                .agg({{AggOp::Count, "", "n"}})
                .collect()
                .get();
        CHECK(pt.num_rows() > 0);  // num_pid_tids
        CHECK(sum_i64(pt, "n") == 50);
    }

    TEST_CASE("StatsView::stats fills the full stats struct") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_mixed_trace(env, 30, 20);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        statistics::ViewStats s = run_stats(statistics::StatsView::from_file(
            dstd::String(gz), dstd::String(idx)));

        CHECK(s.total_events == 50);
        CHECK(s.category_counts.size() == 2);
        std::uint64_t cat_sum = 0;
        for (auto& [k, v] : s.category_counts) cat_sum += v;
        CHECK(cat_sum == 50);

        CHECK(s.name_counts.size() > 0);
        CHECK(s.pid_tid_counts.size() > 0);
        std::uint64_t pt_sum = 0;
        for (auto& [k, v] : s.pid_tid_counts) pt_sum += v;
        CHECK(pt_sum == 50);

        CHECK(s.max_timestamp_us >= s.min_timestamp_us);
        CHECK(s.duration_max_us >= s.duration_min_us);
        CHECK(s.duration_mean() >= 0.0);
        CHECK(s.duration_m2 >= 0.0);  // Welford M2 is non-negative
    }

    TEST_CASE("fill_chunk_statistics populates the report model") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_mixed_trace(env, 30, 20);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        statistics::ViewStats s = run_stats(statistics::StatsView::from_file(
            dstd::String(gz), dstd::String(idx)));
        dftracer::utils::index::schemas::dft::ChunkStatistics cs;
        statistics::fill_chunk_statistics(cs, s);

        CHECK(cs.total_events == 50);
        CHECK(cs.category_counts.size() == 2);
        CHECK(cs.name_counts.size() > 0);
        CHECK(cs.pid_tid_counts.size() > 0);
        CHECK(cs.duration_mean() >= 0.0);
        CHECK(cs.max_timestamp_us >= cs.min_timestamp_us);
        // interned keys survive the source ViewStats going away
        std::uint64_t sum = 0;
        for (const auto& kv : cs.category_counts) sum += kv.second;
        CHECK(sum == 50);
    }

    TEST_CASE("materialize then collect matches a cold collect") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_mixed_trace(env, 30, 20);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        statistics::ViewStats cold = run_stats(statistics::StatsView::from_file(
            dstd::String(gz), dstd::String(idx)));

        // Persist the rollups, then read them back.
        dftracer::utils::Runtime rt;
        rt.block_on(coro::named(
            "materialize",
            coro::with_scope(
                [&](coro::Scope&) -> dftracer::utils::coro::CoroTask<void> {
                    co_await statistics::StatsView::from_file(dstd::String(gz),
                                                              dstd::String(idx))
                        .materialize();
                })));
        statistics::ViewStats warm = run_stats(statistics::StatsView::from_file(
            dstd::String(gz), dstd::String(idx)));

        CHECK(warm.total_events == cold.total_events);
        CHECK(warm.category_counts.size() == cold.category_counts.size());
        CHECK(warm.name_counts.size() == cold.name_counts.size());
        CHECK(warm.pid_tid_counts.size() == cold.pid_tid_counts.size());
        CHECK(warm.min_timestamp_us == cold.min_timestamp_us);
        CHECK(warm.max_timestamp_us == cold.max_timestamp_us);
        CHECK(warm.duration_sum_us == cold.duration_sum_us);
    }

    TEST_CASE("one partition scan materializes several rollups") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_mixed_trace(env, 30, 20);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        dftracer::utils::Runtime rt;
        rt.block_on(coro::named(
            "materialize",
            coro::with_scope(
                [&](coro::Scope&) -> dftracer::utils::coro::CoroTask<void> {
                    dataframe::LazyFrame plan =
                        View::from_file(dstd::String(gz), dstd::String(idx))
                            .branch([](ViewSession& s) {
                                s.materialize({GroupKey::cat()},
                                              {{AggOp::Count, "", "n"}});
                                s.materialize({GroupKey::name()},
                                              {{AggOp::Count, "", "n"}});
                                return dstd::Function<void(const ExportStats&)>(
                                    [](const ExportStats&) {});
                            });
                    co_await plan.collect();
                })));

        // Both rollups are now reconstructable with no scan.
        auto cat = View::from_file(dstd::String(gz), dstd::String(idx))
                       .group_by({GroupKey::cat()})
                       .agg({{AggOp::Count, "", "n"}})
                       .reconstruct_if_cached();
        auto name = View::from_file(dstd::String(gz), dstd::String(idx))
                        .group_by({GroupKey::name()})
                        .agg({{AggOp::Count, "", "n"}})
                        .reconstruct_if_cached();
        REQUIRE(cat.has_value());
        REQUIRE(name.has_value());
        CHECK(sum_i64(*cat, "n") == 50);
        CHECK(sum_i64(*name, "n") == 50);
    }

    TEST_CASE("execute() serves materialized branches without scanning") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_mixed_trace(env, 30, 20);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        dftracer::utils::Runtime rt;
        rt.block_on(coro::named(
            "materialize",
            coro::with_scope(
                [&](coro::Scope&) -> dftracer::utils::coro::CoroTask<void> {
                    dataframe::LazyFrame plan =
                        View::from_file(dstd::String(gz), dstd::String(idx))
                            .branch([](ViewSession& s) {
                                s.materialize({GroupKey::cat()},
                                              {{AggOp::Count, "", "n"}});
                                s.materialize({GroupKey::name()},
                                              {{AggOp::Count, "", "n"}});
                                return dstd::Function<void(const ExportStats&)>(
                                    [](const ExportStats&) {});
                            });
                    co_await plan.collect();
                })));

        ExportStats stats;
        std::int64_t cat_sum = 0;
        rt.block_on(coro::named(
            "collect",
            coro::with_scope(
                [&](coro::Scope&) -> dftracer::utils::coro::CoroTask<void> {
                    dataframe::LazyFrame plan =
                        View::from_file(dstd::String(gz), dstd::String(idx))
                            .branch([&](ViewSession& s) {
                                auto cat = s.collect({GroupKey::cat()},
                                                     {{AggOp::Count, "", "n"}});
                                auto name =
                                    s.collect({GroupKey::name()},
                                              {{AggOp::Count, "", "n"}});
                                return dstd::Function<void(const ExportStats&)>(
                                    [&, cat](const ExportStats& st) {
                                        stats = st;
                                        cat_sum = sum_i64(cat.get(), "n");
                                    });
                            });
                    co_await plan.collect();
                })));

        // Both branches were served from rollups, so nothing was scanned.
        CHECK(stats.chunks_scanned == 0);
        CHECK(stats.events_scanned == 0);
        CHECK(cat_sum == 50);
    }

    TEST_CASE("session aggregation spills under a tiny memory budget") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_mixed_trace(env, 30, 20);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        // A 1-byte budget forces the AggFold to spill every group to a sorted
        // run and k-way merge them back; the result must still be exact.
        dataframe::DataFrame cats;
        dftracer::utils::Runtime rt;
        rt.block_on(coro::named(
            "spill",
            coro::with_scope(
                [&](coro::Scope&) -> dftracer::utils::coro::CoroTask<void> {
                    dataframe::LazyFrame plan =
                        View::from_file(dstd::String(gz), dstd::String(idx))
                            .memory_budget(1)
                            .branch([&](ViewSession& s) {
                                auto c = s.collect({GroupKey::cat()},
                                                   {{AggOp::Count, "", "n"}});
                                return dstd::Function<void(const ExportStats&)>(
                                    [&, c](const ExportStats&) {
                                        cats = std::move(c.get());
                                    });
                            });
                    co_await plan.collect();
                })));
        CHECK(cats.num_rows() == 2);
        CHECK(sum_i64(cats, "n") == 50);
    }

    TEST_CASE("pid/tid counts carry the real pid:tid keys") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        // Distinct pid/tid groups: pid/tid are String group columns, so reading
        // them as int64 would yield garbage keys.
        dstd::String pfw = env.get_dir() + "/pt.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            for (int i = 0; i < 10; ++i)
                ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1000,)"
                       R"("tid":5000,"ts":)"
                    << (1000 + i * 10) << R"(,"dur":10,"args":{}})" << "\n";
            for (int i = 0; i < 6; ++i)
                ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1001,)"
                       R"("tid":5001,"ts":)"
                    << (2000 + i * 10) << R"(,"dur":10,"args":{}})" << "\n";
        }
        dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        (void)dstd::fs::remove(pfw);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        statistics::ViewStats s = run_stats(statistics::StatsView::from_file(
            dstd::String(gz), dstd::String(idx)));
        dstd::Map<dstd::String, std::uint64_t> m;
        for (auto& [k, v] : s.pid_tid_counts) m[dstd::String(k)] = v;
        CHECK(m.count("1000:5000") == 1);
        CHECK(m.count("1001:5001") == 1);
        CHECK(m["1000:5000"] == 10);
        CHECK(m["1001:5001"] == 6);
    }

    TEST_CASE("duration stats and time range") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_mixed_trace(env, 30, 20);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        dataframe::DataFrame d =
            View::from_file(dstd::String(gz), dstd::String(idx))
                .group_by({GroupKey::cat()})
                .agg({{AggOp::Mean, "dur", "mean"},
                      {AggOp::Min, "dur", "dmin"},
                      {AggOp::Max, "dur", "dmax"},
                      {AggOp::Min, "ts", "tmin"},
                      {AggOp::Max, "ts", "tmax"}})
                .collect()
                .get();
        REQUIRE(d.num_rows() == 2);
        const double* mean = d.columns[col(d, "mean")].data<double>();
        const std::int64_t* dmin =
            d.columns[col(d, "dmin")].data<std::int64_t>();
        const std::int64_t* dmax =
            d.columns[col(d, "dmax")].data<std::int64_t>();
        for (std::int64_t i = 0; i < d.num_rows(); ++i) {
            CHECK(dmax[i] >= dmin[i]);
            CHECK(mean[i] >= 0.0);
        }
    }
}
