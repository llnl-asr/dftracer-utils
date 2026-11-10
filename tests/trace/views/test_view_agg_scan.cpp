// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/agg_parity_common.h>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;
using namespace aggparity;

using namespace dftracer::utils::trace;

TEST_SUITE("View") {
    TEST_CASE("View - occupancy engine path matches the GroupMap path") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        // Overlapping intervals per name group: same-name events are 150 us
        // apart but each lasts 200 us, so pairs overlap (busy < sum(dur), peak
        // depth 2) while a third never joins them.
        dstd::String pfw = env.dir() + "/occ_engine.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            const char* names[] = {"read", "write", "open"};
            int ts = 1000;
            for (int i = 0; i < 90; ++i) {
                ofs << R"({"ph":"X","name":")" << names[i % 3]
                    << R"(","cat":"POSIX","pid":1,"tid":10,"ts":)" << ts
                    << R"(,"dur":200,"args":{}})" << "\n";
                ts += 50;
            }
        }
        dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        dstd::fs::remove(pfw).discard();
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        auto collect_engine = [](const views::detail::scan::ScanPlan& v) {
            static dftracer::utils::Runtime rt;
            dataframe::DataFrame result;
            dftu_utils_test::ok(rt.block_on(coro::named(
                "occ-engine",
                coro::with_scope(
                    [&](coro::Scope&) -> dftracer::utils::coro::CoroTask<void> {
                        result = dftu_utils_test::ok(
                            co_await views::detail::run_collect_via_engine(*v));
                    }))));
            return dftu_utils_test::ok(
                views::detail::apply_agg_post_ops(std::move(result), *v));
        };
        auto collect_groupmap = [](const views::detail::scan::ScanPlan& v) {
            return gmoracle::groupmap_oracle(v);
        };
        auto check_match = [](const dataframe::DataFrame& a0,
                              const dataframe::DataFrame& b0,
                              const dstd::String& key) {
            dataframe::DataFrame a =
                dftu_utils_test::ok(a0.sort_by(key, false));
            dataframe::DataFrame b =
                dftu_utils_test::ok(b0.sort_by(key, false));
            REQUIRE(a.names.size() == b.names.size());
            for (std::size_t i = 0; i < a.names.size(); ++i) {
                CHECK(a.names[i] == b.names[i]);
                CHECK(a.columns[i].type() == b.columns[i].type());
            }
            REQUIRE(a.num_rows() == b.num_rows());
            for (std::int64_t r = 0; r < a.num_rows(); ++r)
                for (const auto& name : a.names) {
                    const auto c = static_cast<std::size_t>(bcol(a, name));
                    if (a.columns[c].type() == dataframe::TypeId::String)
                        CHECK(bstr(a, r, name) == bstr(b, r, name));
                    else
                        CHECK(bnum(a, r, name) ==
                              doctest::Approx(bnum(b, r, name)));
                }
        };

        auto run_both = [&](std::uint64_t occ_cell, std::uint64_t mem_budget) {
            auto build = [&] {
                views::detail::scan::ScanPlan v =
                    views::detail::scan::from_file(dstd::String(gz),
                                                   dstd::String(idx));
                if (mem_budget)
                    v = views::detail::scan::memory_budget(v, mem_budget);
                if (occ_cell) v = views::detail::scan::occ_cell(v, occ_cell);
                return views::detail::scan::agg(
                    views::detail::scan::group_by(v, {GroupKey::name()}),
                    {
                        {AggOp::Sum, "dur", "sum_dur"},
                        {AggOp::Busy, "", "busy"},
                        {AggOp::Concurrency, "", "concurrency"},
                        {AggOp::Utilization, "", "utilization"},
                        {AggOp::Active, "", "active"},
                    });
            };
            REQUIRE(!views::detail::scan::is_row_query(build()));
            dataframe::DataFrame legacy = collect_groupmap(build());
            dataframe::DataFrame engine = collect_engine(build());
            check_match(legacy, engine, "name");
            // Overlap invariants on the exact-union engine result: busy is
            // strictly below sum(dur) and the peak overlap depth is 2.
            if (occ_cell == 0)
                for (std::int64_t r = 0; r < engine.num_rows(); ++r) {
                    CHECK(bnum(engine, r, "busy") < bnum(engine, r, "sum_dur"));
                    CHECK(bnum(engine, r, "active") == doctest::Approx(2.0));
                }
        };

        SUBCASE("group_by name, exact union") { run_both(0, 0); }
        SUBCASE("group_by name, exact union, forced spill") {
            run_both(0, 128);
        }
        SUBCASE("group_by name, occ_cell tolerance") { run_both(64, 0); }
        SUBCASE("group_by name, occ_cell tolerance, forced spill") {
            run_both(64, 128);
        }

        // Occupancy alongside a scaled value agg under a non-identity
        // time_scale: occupancy reads RAW ts/dur (a time-native reduction)
        // while Sum/Mean(dur) get the unrounded scaled value - per-column, not
        // a global switch. Both paths must agree.
        auto run_both_scaled = [&](std::uint64_t mem_budget) {
            auto build = [&] {
                views::detail::scan::ScanPlan v =
                    views::detail::scan::from_file(dstd::String(gz),
                                                   dstd::String(idx));
                if (mem_budget)
                    v = views::detail::scan::memory_budget(v, mem_budget);
                return views::detail::scan::agg(
                    views::detail::scan::group_by(
                        views::detail::scan::time_scale(v, 0.001),
                        {GroupKey::name()}),
                    {
                        {AggOp::Busy, "", "busy"},
                        {AggOp::Active, "", "active"},
                        {AggOp::Sum, "dur", "sum_dur"},
                        {AggOp::Mean, "dur", "mean_dur"},
                    });
            };
            REQUIRE(!views::detail::scan::is_row_query(build()));
            check_match(collect_groupmap(build()), collect_engine(build()),
                        "name");
        };
        SUBCASE("occupancy + scaled value agg under time_scale") {
            run_both_scaled(0);
        }
        SUBCASE("occupancy + scaled value agg under time_scale, forced spill") {
            run_both_scaled(128);
        }
    }

    TEST_CASE("View - global aggregation (no group_by) matches GroupMap") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String pfw = env.dir() + "/agg_global.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            const char* names[] = {"read", "write", "open"};
            const char* cats[] = {"POSIX", "STDIO"};
            int ts = 1000;
            for (int i = 0; i < 90; ++i) {
                ofs << R"({"ph":"X","name":")" << names[i % 3] << R"(","cat":")"
                    << cats[i % 2] << R"(","pid":1,"tid":10,"ts":)" << ts
                    << R"(,"dur":)" << (5 + (i % 17)) << R"(,"args":{"bytes":)"
                    << (i % 13) << R"(}})" << "\n";
                ts += 100;
            }
        }
        dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        dstd::fs::remove(pfw).discard();
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        auto build = [&](std::uint64_t mem) {
            views::detail::scan::ScanPlan v = views::detail::scan::from_file(
                dstd::String(gz), dstd::String(idx));
            if (mem) v = views::detail::scan::memory_budget(v, mem);
            return views::detail::scan::agg(
                v, {
                       {AggOp::Count, "", "n"},
                       {AggOp::Sum, "dur", "sum_dur"},
                       {AggOp::Mean, "dur", "mean_dur"},
                       {AggOp::Min, "dur", "min_dur"},
                       {AggOp::Max, "dur", "max_dur"},
                       {AggOp::Var, "dur", "var_dur"},
                       {AggOp::Std, "dur", "std_dur"},
                       {AggOp::Pct, "dur", "p90_dur", "", 0.9},
                       {AggOp::ArgMax, "name", "top_name", "dur"},
                       {AggOp::SetUnion, "cat", "cats"},
                   });
        };
        // A global result is a single keyless row, so pair the one row
        // directly.
        auto cmp_one = [](const dataframe::DataFrame& a,
                          const dataframe::DataFrame& b) {
            REQUIRE(a.names.size() == b.names.size());
            REQUIRE(a.num_rows() == 1);
            REQUIRE(b.num_rows() == 1);
            for (std::size_t i = 0; i < a.names.size(); ++i) {
                CHECK(a.names[i] == b.names[i]);
                CHECK(a.columns[i].type() == b.columns[i].type());
                const auto& name = a.names[i];
                if (a.columns[i].type() == dataframe::TypeId::String)
                    CHECK(bstr(a, 0, name) == bstr(b, 0, name));
                else
                    CHECK(bnum(a, 0, name) ==
                          doctest::Approx(bnum(b, 0, name)));
            }
        };
        SUBCASE("in-memory") {
            cmp_one(gmoracle::groupmap_oracle(build(0)),
                    engine_collect(build(0)));
        }
        SUBCASE("forced spill") {
            cmp_one(gmoracle::groupmap_oracle(build(128)),
                    engine_collect(build(128)));
        }
        // A bare .agg() with no explicit specs is a global count.
        SUBCASE("bare count") {
            auto q = [&] {
                return views::detail::scan::agg(
                    views::detail::scan::from_file(dstd::String(gz),
                                                   dstd::String(idx)),
                    dstd::Vector<AggSpec>{});
            };
            cmp_one(gmoracle::groupmap_oracle(q()), engine_collect(q()));
        }
        SUBCASE("global histogram") {
            auto q = [&] {
                return views::detail::scan::agg(
                    views::detail::scan::from_file(dstd::String(gz),
                                                   dstd::String(idx)),
                    {{AggOp::Hist, "dur", "h"}});
            };
            dataframe::DataFrame a = gmoracle::groupmap_oracle(q());
            dataframe::DataFrame b = engine_collect(q());
            REQUIRE(a.num_rows() == 1);
            REQUIRE(b.num_rows() == 1);
            auto ha = hist_bins(a, 0, "h");
            auto hb = hist_bins(b, 0, "h");
            REQUIRE(ha.size() == hb.size());
            for (std::size_t i = 0; i < ha.size(); ++i) {
                CHECK(ha[i].lower == doctest::Approx(hb[i].lower));
                CHECK(ha[i].upper == doctest::Approx(hb[i].upper));
                CHECK(ha[i].count == hb[i].count);
            }
        }
        // auto_numeric_metrics discovers "bytes" (and the io-cat "size")
        // through the name-collecting fold; the engine must emit the same dyn
        // columns.
        SUBCASE("global auto numeric metrics") {
            auto q = [&] {
                return views::detail::scan::agg_numeric_args(
                    views::detail::scan::from_file(dstd::String(gz),
                                                   dstd::String(idx)));
            };
            frames_equal(gmoracle::groupmap_oracle(q()), engine_collect(q()),
                         {});
        }
    }

    TEST_CASE("View - session collect matches a fresh engine scan") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_mixed_trace(env, 30, 20);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));
        View base = View::from_file(dstd::String(gz), dstd::String(idx));
        const dstd::Vector<AggSpec> specs = {
            {AggOp::Count, "", "n"},
            {AggOp::Sum, "dur", "sum_dur"},
            {AggOp::Mean, "dur", "mean_dur"},
            {AggOp::Pct, "dur", "p90_dur", "", 0.9},
            {AggOp::ArgMax, "name", "top_name", "dur"},
            {AggOp::SetUnion, "cat", "cats"}};

        auto q = [&] {
            return base.group_by({GroupKey::cat(), GroupKey::name()})
                .agg(specs);
        };
        const dataframe::DataFrame fresh =
            engine_collect(views::detail::scan::agg(
                views::detail::scan::group_by(
                    views::detail::scan::from_file(dstd::String(gz),
                                                   dstd::String(idx)),
                    {GroupKey::cat(), GroupKey::name()}),
                specs));

        // Lone aggregation branch: run_session routes it through the engine.
        SUBCASE("lone branch") {
            auto run = base.session();
            auto d = run.collect(q().lazy());
            dftu_utils_test::ok(run.execute().get());
            frames_equal(*d, fresh, {"cat", "name"});
        }
        // A second collect branch forces the fused multi-branch scan (AggFold +
        // to_batch); each branch must still match a fresh scan.
        SUBCASE("two branches over the shared scan") {
            auto run = base.session();
            auto d = run.collect(q().lazy());
            auto d2 = run.collect(base.group_by({GroupKey::pid()})
                                      .agg({{AggOp::Count, "", "n"}})
                                      .lazy());
            dftu_utils_test::ok(run.execute().get());
            frames_equal(*d, fresh, {"cat", "name"});
            frames_equal(*d2,
                         engine_collect(views::detail::scan::agg(
                             views::detail::scan::group_by(
                                 views::detail::scan::from_file(
                                     dstd::String(gz), dstd::String(idx)),
                                 {GroupKey::pid()}),
                             {{AggOp::Count, "", "n"}})),
                         {"pid"});
        }
    }

    TEST_CASE("View - rank harvest resolves identically to the GroupMap path") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String pfw = env.dir() + "/agg_rank.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            for (int pid : {1, 2, 3})
                ofs << R"({"name":"PR","cat":"dftracer","pid":)" << pid
                    << R"(,"tid":1,"ph":"M","args":{"name":"rank","value":")"
                    << (pid * 10) << R"("}})" << "\n";
            int ts = 1000;
            for (int i = 0; i < 60; ++i) {
                ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":)"
                    << (1 + i % 3) << R"(,"tid":10,"ts":)" << ts << R"(,"dur":)"
                    << (5 + i % 9) << R"(,"args":{}})" << "\n";
                ts += 100;
            }
        }
        dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        dstd::fs::remove(pfw).discard();
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        auto q = [&] {
            return views::detail::scan::agg(
                views::detail::scan::group_by(
                    views::detail::scan::from_file(dstd::String(gz),
                                                   dstd::String(idx)),
                    {GroupKey::rank()}),
                {{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "sum_dur"}});
        };
        frames_equal(gmoracle::groupmap_oracle(q()), engine_collect(q()),
                     {"rank"});
    }
}
