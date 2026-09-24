#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "agg_parity_common.h"

namespace scan = dftracer::utils::trace::views::detail::scan;

TEST_SUITE("View") {
    TEST_CASE(
        "Trace scan - agg engine composite keys match the GroupMap path") {
        REQUIRE(parity_env().is_valid());
        const std::string gz = write_agg_engine_trace(parity_sub("base"));
        const std::string idx = determine_index_path(gz, "");

        SUBCASE("group_by (name, pid)") {
            run_both_multi({{gz, idx}}, {GroupKey::name(), GroupKey::pid()},
                           {"name", "pid"}, 0);
        }
        SUBCASE("group_by (name, pid, tid), forced spill") {
            run_both_multi({{gz, idx}},
                           {GroupKey::name(), GroupKey::pid(), GroupKey::tid()},
                           {"name", "pid", "tid"}, 128);
        }
        SUBCASE("group_by (cat, io_cat)") {
            run_both_multi({{gz, idx}}, {GroupKey::cat(), GroupKey::io_cat()},
                           {"cat", "io_cat"}, 0);
        }
        SUBCASE("group_by (cat, io_cat), forced spill") {
            run_both_multi({{gz, idx}}, {GroupKey::cat(), GroupKey::io_cat()},
                           {"cat", "io_cat"}, 128);
        }

        // Two files, so fhash genuinely varies across groups (not just pid).
        const std::string gz2 = parity_sub("two") + "/agg_engine2.pfw.gz";
        const std::string idx2 = determine_index_path(gz2, "");
        const std::string gz2a = write_agg_engine_trace(parity_sub("two"));
        if (!fs::exists(gz2)) {
            std::string pfw2 = parity_sub("two") + "/agg_engine2.pfw";
            std::ofstream ofs(pfw2);
            const char* names[] = {"read", "write", "open"};
            const int pids[] = {1, 2, 3};
            int ts = 1000;
            for (int i = 0; i < 60; ++i) {
                ofs << R"({"ph":"X","name":")" << names[i % 3]
                    << R"(","cat":"POSIX","pid":)" << pids[i % 3]
                    << R"(,"tid":10,"ts":)" << ts << R"(,"dur":)"
                    << (5 + (i % 11)) << R"(,"args":{}})" << "\n";
                ts += 100;
            }
            ofs.close();
            dftu_utils_test::compress_file_to_gzip(pfw2, gz2);
            fs::remove(pfw2);
        }
        SUBCASE("group_by (pid, fhash) across two files") {
            run_both_multi({{gz2a, idx2}, {gz2, idx2}},
                           {GroupKey::pid(), GroupKey::fhash()},
                           {"pid", "fhash"}, 0);
        }
        SUBCASE("group_by (pid, fhash) across two files, forced spill") {
            run_both_multi({{gz2a, idx2}, {gz2, idx2}},
                           {GroupKey::pid(), GroupKey::fhash()},
                           {"pid", "fhash"}, 128);
        }

        // Mixed-case cat values: the GroupMap path lowercases the group key
        // (agg_fold.h's lower_ascii), so "POSIX"/"posix"/"Stdio"/"STDIO" must
        // merge into two groups ("posix", "stdio") in both paths.
        const std::string gz3 = parity_sub("cat") + "/agg_engine_cat.pfw.gz";
        const std::string idx3 = determine_index_path(gz3, "");
        if (!fs::exists(gz3)) {
            std::string pfw3 = parity_sub("cat") + "/agg_engine_cat.pfw";
            std::ofstream ofs(pfw3);
            const char* names[] = {"read", "write", "open"};
            const char* cats[] = {"POSIX", "posix", "Stdio", "STDIO"};
            const int pids[] = {1, 2};
            int ts = 1000;
            for (int i = 0; i < 80; ++i) {
                ofs << R"({"ph":"X","name":")" << names[i % 3] << R"(","cat":")"
                    << cats[i % 4] << R"(","pid":)" << pids[i % 2]
                    << R"(,"tid":10,"ts":)" << ts << R"(,"dur":)"
                    << (5 + (i % 13)) << R"(,"args":{}})" << "\n";
                ts += 100;
            }
            ofs.close();
            dftu_utils_test::compress_file_to_gzip(pfw3, gz3);
            fs::remove(pfw3);
        }
        SUBCASE("group_by cat, mixed case merges") {
            run_both_file(gz3, idx3, GroupKey::cat(), "cat", 0);
        }
        SUBCASE("group_by cat, mixed case merges, forced spill") {
            run_both_file(gz3, idx3, GroupKey::cat(), "cat", 128);
        }
        SUBCASE("group_by (cat, pid), mixed case merges") {
            run_both_multi({{gz3, idx3}}, {GroupKey::cat(), GroupKey::pid()},
                           {"cat", "pid"}, 0);
        }
        SUBCASE("group_by (cat, pid), mixed case merges, forced spill") {
            run_both_multi({{gz3, idx3}}, {GroupKey::cat(), GroupKey::pid()},
                           {"cat", "pid"}, 128);
        }

        // Resolved-name keys (FilePath/FileName/HostName): each file declares
        // its own fhash/hhash via FH/HH metadata and every event references it,
        // so the two files genuinely resolve to different names (distinct
        // basenames, so FileName has no cross-file collision to merge).
        const std::string rdir = parity_sub("resolved");
        const std::string gz4 = rdir + "/agg_engine_resolved_a.pfw.gz";
        const std::string idx4 = determine_index_path(gz4, "");
        const std::string gz5 = rdir + "/agg_engine_resolved_b.pfw.gz";
        const std::string idx5 = determine_index_path(gz5, "");
        if (!fs::exists(gz4) || !fs::exists(gz5)) {
            std::string pfw4 = rdir + "/agg_engine_resolved_a.pfw";
            std::ofstream ofs(pfw4);
            ofs << R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"/data/dirA/a.h5","value":"FA1"}})"
                << "\n"
                << R"({"name":"HH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"nodeA","value":"HA1"}})"
                << "\n";
            const char* names[] = {"read", "write", "open"};
            const int pids[] = {1, 2};
            int ts = 1000;
            for (int i = 0; i < 30; ++i) {
                ofs << R"({"ph":"X","name":")" << names[i % 3]
                    << R"(","cat":"POSIX","pid":)" << pids[i % 2]
                    << R"(,"tid":10,"ts":)" << ts << R"(,"dur":)"
                    << (5 + (i % 11))
                    << R"(,"args":{"fhash":"FA1","hhash":"HA1"}})" << "\n";
                ts += 100;
            }
            ofs.close();
            dftu_utils_test::compress_file_to_gzip(pfw4, gz4);
            fs::remove(pfw4);

            std::string pfw5 = rdir + "/agg_engine_resolved_b.pfw";
            std::ofstream ofs5(pfw5);
            ofs5
                << R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"/data/dirB/b.h5","value":"FB1"}})"
                << "\n"
                << R"({"name":"HH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"nodeB","value":"HB1"}})"
                << "\n";
            ts = 1000;
            for (int i = 0; i < 20; ++i) {
                ofs5 << R"({"ph":"X","name":")" << names[i % 3]
                     << R"(","cat":"POSIX","pid":)" << pids[i % 2]
                     << R"(,"tid":10,"ts":)" << ts << R"(,"dur":)"
                     << (5 + (i % 7))
                     << R"(,"args":{"fhash":"FB1","hhash":"HB1"}})" << "\n";
                ts += 100;
            }
            ofs5.close();
            dftu_utils_test::compress_file_to_gzip(pfw5, gz5);
            fs::remove(pfw5);
        }
        SUBCASE("group_by file_path across two files") {
            run_both_multi({{gz4, idx4}, {gz5, idx5}}, {GroupKey::file_path()},
                           {"file_path"}, 0);
        }
        SUBCASE("group_by file_path across two files, forced spill") {
            run_both_multi({{gz4, idx4}, {gz5, idx5}}, {GroupKey::file_path()},
                           {"file_path"}, 128);
        }
        SUBCASE("group_by file_name across two files") {
            run_both_multi({{gz4, idx4}, {gz5, idx5}}, {GroupKey::file_name()},
                           {"file_name"}, 0);
        }
        SUBCASE("group_by host_name across two files") {
            run_both_multi({{gz4, idx4}, {gz5, idx5}}, {GroupKey::host_name()},
                           {"host_name"}, 0);
        }
        SUBCASE("group_by (file_path, pid) across two files") {
            run_both_multi({{gz4, idx4}, {gz5, idx5}},
                           {GroupKey::file_path(), GroupKey::pid()},
                           {"file_path", "pid"}, 0);
        }
        SUBCASE("group_by (file_path, pid) across two files, forced spill") {
            run_both_multi({{gz4, idx4}, {gz5, idx5}},
                           {GroupKey::file_path(), GroupKey::pid()},
                           {"file_path", "pid"}, 128);
        }

        // Rank is a resolved-like key harvested from PR metadata: each pid is
        // declared once (pid -> rank), events group on pid, and both paths must
        // relabel the pid groups to the same rank strings.
        const std::string gz6 = parity_sub("rank") + "/agg_engine_rank.pfw.gz";
        const std::string idx6 = determine_index_path(gz6, "");
        if (!fs::exists(gz6)) {
            std::string pfw6 = parity_sub("rank") + "/agg_engine_rank.pfw";
            std::ofstream ofs(pfw6);
            ofs << R"({"ph":"M","name":"PR","cat":"dftracer","pid":100,"tid":0,"args":{"name":"rank","value":"0"}})"
                << "\n"
                << R"({"ph":"M","name":"PR","cat":"dftracer","pid":200,"tid":0,"args":{"name":"rank","value":"1"}})"
                << "\n"
                << R"({"ph":"M","name":"PR","cat":"dftracer","pid":300,"tid":0,"args":{"name":"rank","value":"2"}})"
                << "\n";
            const char* names[] = {"read", "write", "open"};
            const int pids[] = {100, 200, 300};
            int ts = 1000;
            for (int i = 0; i < 90; ++i) {
                ofs << R"({"ph":"X","name":")" << names[i % 3]
                    << R"(","cat":"POSIX","pid":)" << pids[i % 3]
                    << R"(,"tid":10,"ts":)" << ts << R"(,"dur":)"
                    << (5 + (i % 17)) << R"(,"args":{}})" << "\n";
                ts += 100;
            }
            ofs.close();
            dftu_utils_test::compress_file_to_gzip(pfw6, gz6);
            fs::remove(pfw6);
        }
        SUBCASE("group_by rank") {
            run_both_file(gz6, idx6, GroupKey::rank(), "rank", 0);
        }
        SUBCASE("group_by rank, forced spill") {
            run_both_file(gz6, idx6, GroupKey::rank(), "rank", 128);
        }

        // An Arg/Field key derives a group-key column from a flattened arg (or,
        // for Field, a top-level-then-arg lookup); both paths must render the
        // same key text, including "" for the events missing the arg. The
        // read events carry arg x, the write events carry arg y, so grouping by
        // x exercises the present-and-missing split.
        const std::string gz7 = parity_sub("arg") + "/agg_engine_arg.pfw.gz";
        const std::string idx7 = determine_index_path(gz7, "");
        if (!fs::exists(gz7)) {
            std::string pfw7 = parity_sub("arg") + "/agg_engine_arg.pfw";
            std::ofstream ofs(pfw7);
            int ts = 1000;
            for (int i = 0; i < 30; ++i) {
                ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,)"
                       R"("tid":10,"ts":)"
                    << ts << R"(,"dur":)" << (5 + (i % 11))
                    << R"(,"args":{"x":)" << (i % 5) << R"(}})" << "\n";
                ts += 100;
            }
            for (int i = 0; i < 30; ++i) {
                ofs << R"({"ph":"X","name":"write","cat":"STDIO","pid":2,)"
                       R"("tid":20,"ts":)"
                    << ts << R"(,"dur":)" << (5 + (i % 13))
                    << R"(,"args":{"y":)" << (i % 4) << R"(}})" << "\n";
                ts += 100;
            }
            ofs.close();
            dftu_utils_test::compress_file_to_gzip(pfw7, gz7);
            fs::remove(pfw7);
        }
        SUBCASE("group_by arg x") {
            run_both_file(gz7, idx7, GroupKey::of_arg("x"), "x", 0);
        }
        SUBCASE("group_by arg x, forced spill") {
            run_both_file(gz7, idx7, GroupKey::of_arg("x"), "x", 128);
        }
        SUBCASE("group_by field name") {
            run_both_file(gz7, idx7, GroupKey::field("name"), "name", 0);
        }
        SUBCASE("group_by field x (arg via field)") {
            run_both_file(gz7, idx7, GroupKey::field("x"), "x", 0);
        }
        SUBCASE("group_by (name, arg x)") {
            run_both_multi({{gz7, idx7}},
                           {GroupKey::name(), GroupKey::of_arg("x")},
                           {"name", "x"}, 0);
        }
        SUBCASE("group_by (name, arg x), forced spill") {
            run_both_multi({{gz7, idx7}},
                           {GroupKey::name(), GroupKey::of_arg("x")},
                           {"name", "x"}, 128);
        }

        // auto_numeric_metrics discovers the numeric args at scan (gz7's read
        // events carry x, write events carry y), so both the bare-mean legacy
        // path and explicit per-arg reductions must emit the same discovered
        // columns (x/y), with 0.0 where an arg never appears in a group.
        // Grouped by name so read has x-present/y-absent and write the reverse.
        auto run_both_dyn =
            [&](const std::function<scan::ScanPlan(scan::ScanPlan)>& agg_of,
                std::uint64_t mem_budget) {
                auto build = [&] {
                    scan::ScanPlan v = scan::from_file(gz7, idx7);
                    if (mem_budget) v = scan::memory_budget(v, mem_budget);
                    return agg_of(scan::group_by(v, {GroupKey::name()}));
                };
                check_match(collect_groupmap_plan(build()),
                            collect_engine_plan(build()), "name");
            };
        SUBCASE("group_by name + auto_numeric_metrics (legacy bare mean)") {
            run_both_dyn(
                [](scan::ScanPlan v) { return scan::agg_numeric_args(v); }, 0);
        }
        SUBCASE("group_by name + auto_numeric_metrics, forced spill") {
            run_both_dyn(
                [](scan::ScanPlan v) { return scan::agg_numeric_args(v); },
                128);
        }
        SUBCASE("group_by name + explicit numeric_arg_aggs") {
            run_both_dyn(
                [](scan::ScanPlan v) {
                    return scan::agg_numeric_args(
                        v, {
                               AggSpec(AggOp::Sum),
                               AggSpec(AggOp::Mean),
                               AggSpec(AggOp::Min),
                               AggSpec(AggOp::Max),
                               AggSpec(AggOp::SumSq),
                               AggSpec(AggOp::Var),
                               AggSpec(AggOp::Std),
                               AggSpec(AggOp::Skew),
                               AggSpec(AggOp::Kurt),
                               AggSpec(AggOp::Pct, "", "p90", "", 0.9),
                           });
                },
                0);
        }
        SUBCASE("group_by name + explicit numeric_arg_aggs, forced spill") {
            run_both_dyn(
                [](scan::ScanPlan v) {
                    return scan::agg_numeric_args(
                        v, {
                               AggSpec(AggOp::Sum),
                               AggSpec(AggOp::Mean),
                               AggSpec(AggOp::Var),
                               AggSpec(AggOp::Pct, "", "p90", "", 0.9),
                           });
                },
                128);
        }
        // A trace whose events carry no numeric args discovers nothing, so the
        // dyn path collapses to just the group count column in both paths.
        SUBCASE("group_by name + auto_numeric_metrics, no numeric args") {
            auto build = [&] {
                return scan::agg_numeric_args(scan::group_by(
                    scan::from_file(gz, idx), {GroupKey::name()}));
            };
            check_match(collect_groupmap_plan(build()),
                        collect_engine_plan(build()), "name");
        }
        // Single-scan dyn: the engine (no name pre-scan) must byte-match the
        // GroupMap oracle for an auto_numeric_metrics plan, in-memory and
        // spilled.
        auto run_single_scan_dyn = [&](std::uint64_t mem_budget) {
            auto build = [&] {
                scan::ScanPlan v = scan::from_file(gz7, idx7);
                if (mem_budget) v = scan::memory_budget(v, mem_budget);
                return scan::agg_numeric_args(
                    scan::group_by(v, {GroupKey::name()}),
                    {AggSpec(AggOp::Mean), AggSpec(AggOp::Sum),
                     AggSpec(AggOp::Min), AggSpec(AggOp::Max)});
            };
            check_match(collect_groupmap_plan(build()),
                        collect_engine_plan(build()), "name");
        };
        SUBCASE("group_by name + auto_numeric single scan") {
            run_single_scan_dyn(0);
        }
        SUBCASE("group_by name + auto_numeric single scan, forced spill") {
            run_single_scan_dyn(128);
        }
        // Dyn coexists with fixed value/text columns and occupancy in one scan:
        // explicit group key + auto-numeric args + an explicit value agg +
        // occupancy, byte-matching the oracle in-memory and spilled.
        auto run_mixed = [&](std::uint64_t mem_budget) {
            auto build = [&] {
                scan::ScanPlan v = scan::from_file(gz7, idx7);
                if (mem_budget) v = scan::memory_budget(v, mem_budget);
                return scan::agg_numeric_args(
                    scan::agg(scan::group_by(v, {GroupKey::name()}),
                              {{AggOp::Sum, "dur", "sum_dur"},
                               {AggOp::Busy, "", "busy"}}),
                    {AggSpec(AggOp::Mean), AggSpec(AggOp::Sum)});
            };
            check_match(collect_groupmap_plan(build()),
                        collect_engine_plan(build()), "name");
        };
        SUBCASE("group_by name + value agg + occupancy + auto_numeric") {
            run_mixed(0);
        }
        SUBCASE(
            "group_by name + value agg + occupancy + auto_numeric, forced "
            "spill") {
            run_mixed(128);
        }

        // time_bucket is a computed key too: the bucket column goes first
        // (agg_fold.h prepends it before plan.group_by), so pair rows by
        // [time_bucket, ...extra_key_cols] the same way check_match_multi
        // pairs any other composite key. `bucket_of` applies time_scale/
        // time_bucket(_min); group_by (if any) runs first, matching the
        // group_by-then-time_bucket order used elsewhere in this file.
        auto run_both_bucket =
            [&](const std::function<scan::ScanPlan(scan::ScanPlan)>& bucket_of,
                const std::vector<GroupKey>& extra_gks,
                const std::vector<std::string>& extra_key_cols,
                std::uint64_t mem_budget) {
                auto build = [&] {
                    scan::ScanPlan v = scan::from_file(gz, idx);
                    if (mem_budget) v = scan::memory_budget(v, mem_budget);
                    scan::ScanPlan grouped =
                        extra_gks.empty() ? v : scan::group_by(v, extra_gks);
                    return scan::agg(bucket_of(grouped),
                                     {
                                         {AggOp::Count, "", "n"},
                                         {AggOp::Sum, "dur", "sum_dur"},
                                         {AggOp::Mean, "dur", "mean_dur"},
                                     });
                };
                // Warm the on-disk index once so legacy and engine below see
                // the same (already-built) index: bucket_origin_min reads its
                // zone maps and falls back to origin 0 on a first-touch/missing
                // index, so comparing a first-touch run against a second-touch
                // run would compare two different origins, not the same
                // formula.
                scan::collect(build()).collect().get();
                dataframe::DataFrame legacy = collect_groupmap_plan(build());
                dataframe::DataFrame engine = collect_engine_plan(build());
                std::vector<std::string> keys = {"time_bucket"};
                keys.insert(keys.end(), extra_key_cols.begin(),
                            extra_key_cols.end());
                check_match_multi(legacy, engine, keys);
            };

        SUBCASE("time_bucket alone") {
            run_both_bucket(
                [](scan::ScanPlan v) { return scan::time_bucket(v, 1000); }, {},
                {}, 0);
        }
        SUBCASE("time_bucket alone, forced spill") {
            run_both_bucket(
                [](scan::ScanPlan v) { return scan::time_bucket(v, 1000); }, {},
                {}, 128);
        }
        SUBCASE("time_bucket + name") {
            run_both_bucket(
                [](scan::ScanPlan v) { return scan::time_bucket(v, 1000); },
                {GroupKey::name()}, {"name"}, 0);
        }
        SUBCASE("time_bucket + name, forced spill") {
            run_both_bucket(
                [](scan::ScanPlan v) { return scan::time_bucket(v, 1000); },
                {GroupKey::name()}, {"name"}, 128);
        }
        SUBCASE("time_bucket with an explicit origin") {
            run_both_bucket(
                [](scan::ScanPlan v) { return scan::time_bucket(v, 700, 500); },
                {}, {}, 0);
        }
        SUBCASE("time_bucket_min (trace-min-aligned origin)") {
            run_both_bucket(
                [](scan::ScanPlan v) { return scan::time_bucket_min(v, 700); },
                {}, {}, 0);
        }
        SUBCASE("time_bucket with a non-1.0 time_scale") {
            run_both_bucket(
                [](scan::ScanPlan v) {
                    return scan::time_bucket(scan::time_scale(v, 0.01), 10);
                },
                {}, {}, 0);
        }

        auto build_postop_base = [&] {
            return scan::agg(
                scan::group_by(scan::from_file(gz, idx), {GroupKey::name()}),
                {
                    {AggOp::Count, "", "n"},
                    {AggOp::Sum, "dur", "sum_dur"},
                });
        };
        SUBCASE("group_by name + sort_by") {
            auto build = [&] {
                return scan::sort_by(build_postop_base(), "sum_dur");
            };
            check_match(collect_groupmap_plan(build()),
                        collect_engine_plan(build()), "name");
        }
        SUBCASE("group_by name + topk") {
            auto build = [&] {
                return scan::topk(build_postop_base(), "sum_dur", 2);
            };
            dataframe::DataFrame legacy = collect_groupmap_plan(build());
            dataframe::DataFrame engine = collect_engine_plan(build());
            REQUIRE(legacy.num_rows() == 2);
            check_match(legacy, engine, "name");
        }
        SUBCASE("group_by name + offset/limit") {
            auto build = [&] {
                return scan::limit(
                    scan::offset(scan::sort_by(build_postop_base(), "name"), 1),
                    1);
            };
            dataframe::DataFrame legacy = collect_groupmap_plan(build());
            dataframe::DataFrame engine = collect_engine_plan(build());
            REQUIRE(legacy.num_rows() == 1);
            check_match(legacy, engine, "name");
        }
        SUBCASE("group_by name + select") {
            auto build = [&] {
                return scan::select(build_postop_base(), {"name", "sum_dur"});
            };
            dataframe::DataFrame legacy = collect_groupmap_plan(build());
            dataframe::DataFrame engine = collect_engine_plan(build());
            REQUIRE(legacy.names.size() == 2);
            check_match(legacy, engine, "name");
        }

        // Engine Hist must match GroupMap bin for bin, including under forced
        // spill - the k-way merge path that once could not concat the nested
        // column. Rows are paired by cat text (not sort_by, which would take()
        // the nested column) so a differing group order still compares right.
        auto run_both_hist = [&](std::uint64_t mem_budget) {
            auto build = [&] {
                scan::ScanPlan v = scan::from_file(gz, idx);
                if (mem_budget) v = scan::memory_budget(v, mem_budget);
                return scan::agg(
                    scan::group_by(v, {GroupKey::cat()}),
                    {{AggOp::Count, "", "n"}, {AggOp::Hist, "dur", "h"}});
            };
            dataframe::DataFrame legacy = collect_groupmap_plan(build());
            dataframe::DataFrame engine = collect_engine_plan(build());
            REQUIRE(legacy.num_rows() == engine.num_rows());
            for (std::int64_t lr = 0; lr < legacy.num_rows(); ++lr) {
                const std::string cat = bstr(legacy, lr, "cat");
                std::int64_t er = -1;
                for (std::int64_t r = 0; r < engine.num_rows(); ++r)
                    if (bstr(engine, r, "cat") == cat) {
                        er = r;
                        break;
                    }
                REQUIRE(er >= 0);
                CHECK(bnum(legacy, lr, "n") ==
                      doctest::Approx(bnum(engine, er, "n")));
                const auto lb = hist_bins(legacy, lr, "h");
                const auto eb = hist_bins(engine, er, "h");
                REQUIRE(lb.size() == eb.size());
                for (std::size_t i = 0; i < lb.size(); ++i) {
                    CHECK(lb[i].lower == doctest::Approx(eb[i].lower));
                    CHECK(lb[i].upper == doctest::Approx(eb[i].upper));
                    CHECK(lb[i].count == eb[i].count);
                }
            }
        };
        SUBCASE("group_by cat + Hist") { run_both_hist(0); }
        SUBCASE("group_by cat + Hist, forced spill") { run_both_hist(1); }

        // materialize() through the engine's collect_frame path persists the
        // rollup (the side effect this phase added), so a later coarser query
        // is served by re-aggregating that rollup (find_subsuming_rollup) and
        // must match a fresh pre-rollup scan. Distinct from the subcase above
        // that materializes via the legacy .run() terminal.
        SUBCASE(
            "engine materialize() persists a rollup a coarser query reads") {
            auto fine = [&] {
                return scan::agg(
                    scan::group_by(scan::from_file(gz, idx),
                                   {GroupKey::cat(), GroupKey::name()}),
                    {{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "sum_dur"}});
            };
            auto coarse = [&] {
                return scan::agg(
                    scan::group_by(scan::from_file(gz, idx), {GroupKey::cat()}),
                    {{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "sum_dur"}});
            };
            dataframe::DataFrame expect = collect_groupmap_plan(coarse());
            // The plan collect routes through collect_frame, where the engine
            // materialize persist lives (collect_engine bypasses it).
            scan::collect(scan::materialize(fine(), 0, 0)).collect().get();
            dataframe::DataFrame engine_served = collect_engine_plan(coarse());
            check_match(expect, engine_served, "cat");
        }

        auto with_tf = [](GroupKey g, GroupKey::Transform t,
                          std::vector<std::string> a = {}) {
            g.transform = t;
            g.transform_args = std::move(a);
            return g;
        };

        // Resolved-name key transforms: dirname/basename apply to the resolved
        // path (gz4 -> /data/dirA/a.h5, gz5 -> /data/dirB/b.h5), so both paths
        // must resolve then transform to the same key text.
        SUBCASE("group_by file_path + basename transform") {
            run_both_multi(
                {{gz4, idx4}, {gz5, idx5}},
                {with_tf(GroupKey::file_path(), GroupKey::Transform::Basename)},
                {"file_path"}, 0);
        }
        SUBCASE("group_by file_path + dirname transform") {
            run_both_multi(
                {{gz4, idx4}, {gz5, idx5}},
                {with_tf(GroupKey::file_path(), GroupKey::Transform::Dirname)},
                {"file_path"}, 0);
        }
        SUBCASE("group_by file_path + basename transform, forced spill") {
            run_both_multi(
                {{gz4, idx4}, {gz5, idx5}},
                {with_tf(GroupKey::file_path(), GroupKey::Transform::Basename)},
                {"file_path"}, 128);
        }

        // A dirname transform coarsens: two distinct file hashes in the same
        // directory fold to one key, so both paths must MERGE their events into
        // a single group (a relabel-after-aggregate would leave two rows).
        const std::string ddir = parity_sub("samedir");
        const std::string gz8 = ddir + "/agg_engine_dir.pfw.gz";
        const std::string idx8 = determine_index_path(gz8, "");
        if (!fs::exists(gz8)) {
            std::string pfw8 = ddir + "/agg_engine_dir.pfw";
            std::ofstream ofs(pfw8);
            ofs << R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"/data/shared/f1.h5","value":"FS1"}})"
                << "\n"
                << R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"/data/shared/f2.h5","value":"FS2"}})"
                << "\n";
            const char* fhs[] = {"FS1", "FS2"};
            const char* names[] = {"read", "write", "open"};
            int ts = 1000;
            for (int i = 0; i < 40; ++i) {
                ofs << R"({"ph":"X","name":")" << names[i % 3]
                    << R"(","cat":"POSIX","pid":1,"tid":10,"ts":)" << ts
                    << R"(,"dur":)" << (5 + (i % 11)) << R"(,"args":{"fhash":")"
                    << fhs[i % 2] << R"("}})" << "\n";
                ts += 100;
            }
            ofs.close();
            dftu_utils_test::compress_file_to_gzip(pfw8, gz8);
            fs::remove(pfw8);
        }
        SUBCASE(
            "group_by file_path + dirname transform merges same-dir files") {
            run_both_file(
                gz8, idx8,
                with_tf(GroupKey::file_path(), GroupKey::Transform::Dirname),
                "file_path", 0);
        }
        SUBCASE("group_by file_path + dirname merge, forced spill") {
            run_both_file(
                gz8, idx8,
                with_tf(GroupKey::file_path(), GroupKey::Transform::Dirname),
                "file_path", 128);
        }

        // A bucket transform maps a value to the first matching substring
        // (empty when none matches) over the lowercased cat key; "o" matches
        // both "posix" and "stdio", so gz3's four mixed-case cats coarsen to a
        // single "o" group that both paths must merge identically.
        SUBCASE("group_by cat + bucket transform coarsens to one group") {
            run_both_file(
                gz3, idx3,
                with_tf(GroupKey::cat(), GroupKey::Transform::Bucket, {"o"}),
                "cat", 0);
        }
        SUBCASE("group_by cat + bucket transform, forced spill") {
            run_both_file(gz3, idx3,
                          with_tf(GroupKey::cat(), GroupKey::Transform::Bucket,
                                  {"posix"}),
                          "cat", 128);
        }

        // Count(field) counts only field-present rows (gz7: read carries x,
        // write carries y), unlike Count() which is the group row count.
        SUBCASE("group_by name + Count over a sometimes-absent field") {
            auto build = [&] {
                return scan::agg(scan::group_by(scan::from_file(gz7, idx7),
                                                {GroupKey::name()}),
                                 {{AggOp::Count, "", "n"},
                                  {AggOp::Count, "x", "n_x"},
                                  {AggOp::Count, "y", "n_y"}});
            };
            check_match(collect_groupmap_plan(build()),
                        collect_engine_plan(build()), "name");
        }
        SUBCASE("group_by name + Count(field), forced spill") {
            auto build = [&] {
                return scan::agg(
                    scan::group_by(
                        scan::memory_budget(scan::from_file(gz7, idx7), 128),
                        {GroupKey::name()}),
                    {{AggOp::Count, "", "n"}, {AggOp::Count, "x", "n_x"}});
            };
            check_match(collect_groupmap_plan(build()),
                        collect_engine_plan(build()), "name");
        }
        // dyn Count is the same per-arg present count (Float64 in both paths).
        SUBCASE("group_by name + dyn Count/Sum") {
            auto build = [&] {
                return scan::agg_numeric_args(
                    scan::group_by(scan::from_file(gz7, idx7),
                                   {GroupKey::name()}),
                    {AggSpec(AggOp::Count), AggSpec(AggOp::Sum)});
            };
            check_match(collect_groupmap_plan(build()),
                        collect_engine_plan(build()), "name");
        }

        // Aggregates over an arbitrary arg value field. `v` is present in every
        // group so Sum keeps its exact integer domain in both paths; Mean is
        // Float64 and ArgMax reprs the arg's raw value.
        const std::string gz9 =
            parity_sub("argval") + "/agg_engine_argval.pfw.gz";
        const std::string idx9 = determine_index_path(gz9, "");
        if (!fs::exists(gz9)) {
            std::string pfw9 = parity_sub("argval") + "/agg_engine_argval.pfw";
            std::ofstream ofs(pfw9);
            const char* names[] = {"read", "write"};
            int ts = 1000;
            for (int i = 0; i < 60; ++i) {
                ofs << R"({"ph":"X","name":")" << names[i % 2]
                    << R"(","cat":"POSIX","pid":1,"tid":10,"ts":)" << ts
                    << R"(,"dur":)" << (5 + (i % 13)) << R"(,"args":{"v":)"
                    << (i % 7) << R"(}})" << "\n";
                ts += 100;
            }
            ofs.close();
            dftu_utils_test::compress_file_to_gzip(pfw9, gz9);
            fs::remove(pfw9);
        }
        SUBCASE("group_by name + Sum/Mean/ArgMax over an arg value field") {
            auto build = [&] {
                return scan::agg(scan::group_by(scan::from_file(gz9, idx9),
                                                {GroupKey::name()}),
                                 {{AggOp::Sum, "v", "sum_v"},
                                  {AggOp::Mean, "v", "mean_v"},
                                  {AggOp::ArgMax, "v", "top_v", "dur"}});
            };
            check_match(collect_groupmap_plan(build()),
                        collect_engine_plan(build()), "name");
        }
        SUBCASE("group_by name + arg value-field aggs, forced spill") {
            auto build = [&] {
                return scan::agg(
                    scan::group_by(
                        scan::memory_budget(scan::from_file(gz9, idx9), 128),
                        {GroupKey::name()}),
                    {{AggOp::Sum, "v", "sum_v"}, {AggOp::Mean, "v", "mean_v"}});
            };
            check_match(collect_groupmap_plan(build()),
                        collect_engine_plan(build()), "name");
        }

        // Fold-derived value fields as agg targets: "size" is the io-cat byte
        // size (POSIX read/write ret) and "te" is ts+dur, both computed by the
        // fold. The engine projects each as a typed U64 column, so Sum/Min/Max
        // match GroupMap column-for-column. ret>0 on every event, so size is
        // present in both groups (no all-absent group to widen).
        const std::string gz_sz =
            parity_sub("size") + "/agg_engine_size.pfw.gz";
        const std::string idx_sz = determine_index_path(gz_sz, "");
        if (!fs::exists(gz_sz)) {
            std::string pfw_sz = parity_sub("size") + "/agg_engine_size.pfw";
            std::ofstream ofs(pfw_sz);
            const char* names[] = {"read", "write"};
            int ts = 1000;
            for (int i = 0; i < 60; ++i) {
                ofs << R"({"ph":"X","name":")" << names[i % 2]
                    << R"(","cat":"POSIX","pid":1,"tid":10,"ts":)" << ts
                    << R"(,"dur":)" << (5 + (i % 13)) << R"(,"args":{"ret":)"
                    << (100 + (i % 7) * 10) << R"(}})" << "\n";
                ts += 100;
            }
            ofs.close();
            dftu_utils_test::compress_file_to_gzip(pfw_sz, gz_sz);
            fs::remove(pfw_sz);
        }
        SUBCASE("group_by name + Sum/Min/Max over derived size and te") {
            auto build = [&] {
                return scan::agg(scan::group_by(scan::from_file(gz_sz, idx_sz),
                                                {GroupKey::name()}),
                                 {{AggOp::Sum, "size", "sum_size"},
                                  {AggOp::Min, "size", "min_size"},
                                  {AggOp::Max, "size", "max_size"},
                                  {AggOp::Sum, "te", "sum_te"},
                                  {AggOp::Min, "te", "min_te"},
                                  {AggOp::Max, "te", "max_te"}});
            };
            REQUIRE(!scan::is_row_query(build()));
            check_match(collect_groupmap_plan(build()),
                        collect_engine_plan(build()), "name");
        }
        SUBCASE("derived size/te aggs, forced spill") {
            auto build = [&] {
                return scan::agg(
                    scan::group_by(scan::memory_budget(
                                       scan::from_file(gz_sz, idx_sz), 128),
                                   {GroupKey::name()}),
                    {{AggOp::Sum, "size", "sum_size"},
                     {AggOp::Sum, "te", "sum_te"}});
            };
            check_match(collect_groupmap_plan(build()),
                        collect_engine_plan(build()), "name");
        }

        // A nested value path (args.n.v) is resolved by the GroupMap fold via
        // number_typed; the engine projects it as a value column, resolving the
        // full-name-captured arg exactly like PodSource::find_arg. Present in
        // every event, so both paths keep the exact integer domain.
        const std::string gz_nv =
            parity_sub("nested") + "/agg_engine_nested.pfw.gz";
        const std::string idx_nv = determine_index_path(gz_nv, "");
        if (!fs::exists(gz_nv)) {
            std::string pfw_nv =
                parity_sub("nested") + "/agg_engine_nested.pfw";
            std::ofstream ofs(pfw_nv);
            const char* names[] = {"read", "write"};
            int ts = 1000;
            for (int i = 0; i < 60; ++i) {
                ofs << R"({"ph":"X","name":")" << names[i % 2]
                    << R"(","cat":"POSIX","pid":1,"tid":10,"ts":)" << ts
                    << R"(,"dur":)" << (5 + (i % 13)) << R"(,"args":{"n":{"v":)"
                    << (i % 9) << R"(}}})" << "\n";
                ts += 100;
            }
            ofs.close();
            dftu_utils_test::compress_file_to_gzip(pfw_nv, gz_nv);
            fs::remove(pfw_nv);
        }
        SUBCASE("group_by name + Sum/Mean/ArgMax over a nested value path") {
            auto build = [&] {
                return scan::agg(
                    scan::group_by(scan::from_file(gz_nv, idx_nv),
                                   {GroupKey::name()}),
                    {{AggOp::Sum, "args.n.v", "sum_nv"},
                     {AggOp::Mean, "args.n.v", "mean_nv"},
                     {AggOp::ArgMax, "args.n.v", "top_nv", "dur"}});
            };
            REQUIRE(!scan::is_row_query(build()));
            check_match(collect_groupmap_plan(build()),
                        collect_engine_plan(build()), "name");
        }
        SUBCASE("nested value path aggs, forced spill") {
            auto build = [&] {
                return scan::agg(
                    scan::group_by(scan::memory_budget(
                                       scan::from_file(gz_nv, idx_nv), 128),
                                   {GroupKey::name()}),
                    {{AggOp::Sum, "args.n.v", "sum_nv"},
                     {AggOp::Mean, "args.n.v", "mean_nv"}});
            };
            check_match(collect_groupmap_plan(build()),
                        collect_engine_plan(build()), "name");
        }

        // Intentional divergence (the dropped GroupMap quirk): a domain-
        // sensitive reduction (Sum) over an int arg absent from an ENTIRE
        // group. GroupMap widens the whole column to Float64 because the
        // all-absent group demotes to the F64 default; the engine keeps the
        // field's stable natural type (Int64). The values still agree, so the
        // type is asserted directly rather than check_match-ed against
        // GroupMap. gz7's read events carry x and write events do not, so the
        // write group is all-absent.
        SUBCASE("Sum over an int arg absent from a group keeps engine's type") {
            auto build = [&] {
                return scan::agg(scan::group_by(scan::from_file(gz7, idx7),
                                                {GroupKey::name()}),
                                 {{AggOp::Sum, "x", "sum_x"}});
            };
            REQUIRE(!scan::is_row_query(build()));
            dataframe::DataFrame legacy = collect_groupmap_plan(build());
            dataframe::DataFrame engine = collect_engine_plan(build());
            CHECK(
                engine.columns[static_cast<std::size_t>(bcol(engine, "sum_x"))]
                    .type() == dataframe::TypeId::Int64);
            CHECK(
                legacy.columns[static_cast<std::size_t>(bcol(legacy, "sum_x"))]
                    .type() == dataframe::TypeId::Float64);
            dataframe::DataFrame a = legacy.sort_by("name", false);
            dataframe::DataFrame b = engine.sort_by("name", false);
            REQUIRE(a.num_rows() == b.num_rows());
            for (std::int64_t r = 0; r < a.num_rows(); ++r)
                CHECK(bnum(a, r, "sum_x") ==
                      doctest::Approx(bnum(b, r, "sum_x")));
        }
    }
}
