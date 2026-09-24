#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "agg_parity_common.h"

namespace scan = dftracer::utils::trace::views::detail::scan;

TEST_SUITE("View") {
    TEST_CASE("View - occupancy engine path matches the GroupMap path") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        // Overlapping intervals per name group: same-name events are 150 us
        // apart but each lasts 200 us, so pairs overlap (busy < sum(dur), peak
        // depth 2) while a third never joins them.
        std::string pfw = env.get_dir() + "/occ_engine.pfw";
        {
            std::ofstream ofs(pfw);
            const char* names[] = {"read", "write", "open"};
            int ts = 1000;
            for (int i = 0; i < 90; ++i) {
                ofs << R"({"ph":"X","name":")" << names[i % 3]
                    << R"(","cat":"POSIX","pid":1,"tid":10,"ts":)" << ts
                    << R"(,"dur":200,"args":{}})" << "\n";
                ts += 50;
            }
        }
        std::string gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        fs::remove(pfw);
        std::string idx = determine_index_path(gz, "");

        namespace detail = dftracer::utils::trace::views::detail;
        auto collect_engine = [](const scan::ScanPlan& v) {
            static dftracer::utils::Runtime rt;
            dataframe::DataFrame result;
            rt.run_blocking("occ-engine",
                            [&](dftracer::utils::CoroScope&)
                                -> dftracer::utils::coro::CoroTask<void> {
                                result =
                                    co_await detail::run_collect_via_engine(*v);
                            });
            return detail::apply_agg_post_ops(std::move(result), *v);
        };
        auto collect_groupmap = [](const scan::ScanPlan& v) {
            return groupmap_oracle(v);
        };
        auto check_match = [](const dataframe::DataFrame& a0,
                              const dataframe::DataFrame& b0,
                              const std::string& key) {
            dataframe::DataFrame a = a0.sort_by(key, false);
            dataframe::DataFrame b = b0.sort_by(key, false);
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
                scan::ScanPlan v = scan::from_file(gz, idx);
                if (mem_budget) v = scan::memory_budget(v, mem_budget);
                if (occ_cell) v = scan::occ_cell(v, occ_cell);
                return scan::agg(scan::group_by(v, {GroupKey::name()}),
                                 {
                                     {AggOp::Sum, "dur", "sum_dur"},
                                     {AggOp::Busy, "", "busy"},
                                     {AggOp::Concurrency, "", "concurrency"},
                                     {AggOp::Utilization, "", "utilization"},
                                     {AggOp::Active, "", "active"},
                                 });
            };
            REQUIRE(!scan::is_row_query(build()));
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
                scan::ScanPlan v = scan::from_file(gz, idx);
                if (mem_budget) v = scan::memory_budget(v, mem_budget);
                return scan::agg(scan::group_by(scan::time_scale(v, 0.001),
                                                {GroupKey::name()}),
                                 {
                                     {AggOp::Busy, "", "busy"},
                                     {AggOp::Active, "", "active"},
                                     {AggOp::Sum, "dur", "sum_dur"},
                                     {AggOp::Mean, "dur", "mean_dur"},
                                 });
            };
            REQUIRE(!scan::is_row_query(build()));
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
        std::string pfw = env.get_dir() + "/agg_global.pfw";
        {
            std::ofstream ofs(pfw);
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
        std::string gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        fs::remove(pfw);
        std::string idx = determine_index_path(gz, "");

        auto build = [&](std::uint64_t mem) {
            scan::ScanPlan v = scan::from_file(gz, idx);
            if (mem) v = scan::memory_budget(v, mem);
            return scan::agg(v, {
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
            cmp_one(groupmap_oracle(build(0)), engine_collect(build(0)));
        }
        SUBCASE("forced spill") {
            cmp_one(groupmap_oracle(build(128)), engine_collect(build(128)));
        }
        // A bare .agg() with no explicit specs is a global count.
        SUBCASE("bare count") {
            auto q = [&] {
                return scan::agg(scan::from_file(gz, idx),
                                 std::vector<AggSpec>{});
            };
            cmp_one(groupmap_oracle(q()), engine_collect(q()));
        }
        SUBCASE("global histogram") {
            auto q = [&] {
                return scan::agg(scan::from_file(gz, idx),
                                 {{AggOp::Hist, "dur", "h"}});
            };
            dataframe::DataFrame a = groupmap_oracle(q());
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
                return scan::agg_numeric_args(scan::from_file(gz, idx));
            };
            frames_equal(groupmap_oracle(q()), engine_collect(q()), {});
        }
    }

    TEST_CASE("View - session collect matches a fresh engine scan") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 30, 20);
        std::string idx = determine_index_path(gz, "");
        View base = View::from_file(gz, idx);
        const std::vector<AggSpec> specs = {
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
        const dataframe::DataFrame fresh = engine_collect(
            scan::agg(scan::group_by(scan::from_file(gz, idx),
                                     {GroupKey::cat(), GroupKey::name()}),
                      specs));

        // Lone aggregation branch: run_session routes it through the engine.
        SUBCASE("lone branch") {
            auto run = base.session();
            auto d = run.collect(q().lazy());
            run.execute().get();
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
            run.execute().get();
            frames_equal(*d, fresh, {"cat", "name"});
            frames_equal(
                *d2,
                engine_collect(scan::agg(
                    scan::group_by(scan::from_file(gz, idx), {GroupKey::pid()}),
                    {{AggOp::Count, "", "n"}})),
                {"pid"});
        }
    }

    TEST_CASE("View - rank harvest resolves identically to the GroupMap path") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string pfw = env.get_dir() + "/agg_rank.pfw";
        {
            std::ofstream ofs(pfw);
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
        std::string gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        fs::remove(pfw);
        std::string idx = determine_index_path(gz, "");

        auto q = [&] {
            return scan::agg(
                scan::group_by(scan::from_file(gz, idx), {GroupKey::rank()}),
                {{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "sum_dur"}});
        };
        frames_equal(groupmap_oracle(q()), engine_collect(q()), {"rank"});
    }
}
