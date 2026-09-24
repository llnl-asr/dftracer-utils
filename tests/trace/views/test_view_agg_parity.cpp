#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "agg_parity_common.h"

namespace scan = dftracer::utils::trace::views::detail::scan;

TEST_SUITE("View") {
    TEST_CASE("View - agg engine path matches the GroupMap path") {
        REQUIRE(parity_env().is_valid());
        const std::string gz = write_agg_engine_trace(parity_sub("base"));
        const std::string idx = determine_index_path(gz, "");
        auto run_both = [&](const GroupKey& gk, const std::string& key_col,
                            std::uint64_t mem_budget) {
            run_both_file(gz, idx, gk, key_col, mem_budget);
        };

        SUBCASE("group_by name") { run_both(GroupKey::name(), "name", 0); }
        SUBCASE("group_by pid") { run_both(GroupKey::pid(), "pid", 0); }
        SUBCASE("group_by tid") { run_both(GroupKey::tid(), "tid", 0); }
        SUBCASE("group_by name, forced spill") {
            run_both(GroupKey::name(), "name", 128);
        }
        // io_cat is a computed key: the event name maps to the dfanalyzer I/O
        // category enum (read -> READ, write -> WRITE, open -> METADATA), so
        // both paths must derive and label the same integer categories.
        SUBCASE("group_by io_cat") {
            run_both(GroupKey::io_cat(), "io_cat", 0);
        }
        SUBCASE("group_by io_cat, forced spill") {
            run_both(GroupKey::io_cat(), "io_cat", 128);
        }
        // acc_pat is a stub key: the GroupMap fold emits the constant "0" for
        // every event, so both paths must collapse to a single group whose
        // "acc_pat" value is "0".
        SUBCASE("group_by acc_pat") {
            run_both(GroupKey::acc_pat(), "acc_pat", 0);
        }
        SUBCASE("group_by acc_pat, forced spill") {
            run_both(GroupKey::acc_pat(), "acc_pat", 128);
        }

        // The engine path must try the no-scan tier/rollup fast path before
        // scanning: materialize a finer (cat, name) rollup via the legacy
        // path, then run the coarser (cat) query through the engine. A hit
        // re-aggregates the persisted rollup (find_subsuming_rollup), never
        // rescanning the trace, and must match a fresh (pre-rollup) scan.
        SUBCASE("engine path is served by a subsuming rollup, not a rescan") {
            TestEnvironment renv(200);
            REQUIRE(renv.is_valid());
            const std::string rgz = write_agg_engine_trace(renv.get_dir());
            const std::string ridx = determine_index_path(rgz, "");
            auto fine = [&] {
                return scan::agg(
                    scan::group_by(scan::from_file(rgz, ridx),
                                   {GroupKey::cat(), GroupKey::name()}),
                    {{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "sum_dur"}});
            };
            auto coarse = [&] {
                return scan::agg(
                    scan::group_by(scan::from_file(rgz, ridx),
                                   {GroupKey::cat()}),
                    {{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "sum_dur"}});
            };

            dataframe::DataFrame expect = collect_groupmap_plan(coarse());
            scan::run(fine()).get();  // materialize only the finer rollup
            dataframe::DataFrame engine_served = collect_engine_plan(coarse());
            check_match(expect, engine_served, "cat");
        }

        // A same-grain rollup must serve every reduction, the DDSketch (Pct),
        // SetUnion and ArgMax byte-for-byte (agg_regroup identity), matching a
        // fresh scan.
        SUBCASE("subsuming rollup: same-grain full vocab matches a scan") {
            TestEnvironment renv(200);
            REQUIRE(renv.is_valid());
            const std::string rgz = write_agg_engine_trace(renv.get_dir());
            const std::string ridx = determine_index_path(rgz, "");
            auto q = [&] {
                return scan::agg(scan::group_by(scan::from_file(rgz, ridx),
                                                {GroupKey::cat()}),
                                 {
                                     {AggOp::Count, "", "n"},
                                     {AggOp::Sum, "dur", "sum_dur"},
                                     {AggOp::Mean, "dur", "mean_dur"},
                                     {AggOp::Min, "dur", "min_dur"},
                                     {AggOp::Max, "dur", "max_dur"},
                                     {AggOp::Var, "dur", "var_dur"},
                                     {AggOp::Std, "dur", "std_dur"},
                                     {AggOp::Pct, "dur", "p90_dur", "", 0.9},
                                     {AggOp::SetUnion, "cat", "cats"},
                                     {AggOp::ArgMax, "name", "top_name", "dur"},
                                 });
            };
            dataframe::DataFrame expect = collect_groupmap_plan(q());
            scan::run(q()).get();
            check_match(expect, collect_engine_plan(q()), "cat");
        }

        // Coarsening a finer rollup must re-aggregate the sketch (Pct) and the
        // distinct-value set (SetUnion) exactly (ArgMax is excluded: its
        // representative on a by-value tie is order-dependent across a merge).
        SUBCASE("subsuming rollup: coarsening Pct/SetUnion matches a scan") {
            TestEnvironment renv(200);
            REQUIRE(renv.is_valid());
            const std::string rgz = write_agg_engine_trace(renv.get_dir());
            const std::string ridx = determine_index_path(rgz, "");
            auto specs = [] {
                return std::vector<AggSpec>{
                    {AggOp::Count, "", "n"},
                    {AggOp::Sum, "dur", "sum_dur"},
                    {AggOp::Mean, "dur", "mean_dur"},
                    {AggOp::Var, "dur", "var_dur"},
                    {AggOp::Pct, "dur", "p90_dur", "", 0.9},
                    {AggOp::SetUnion, "name", "names"}};
            };
            auto fine = [&] {
                return scan::agg(
                    scan::group_by(scan::from_file(rgz, ridx),
                                   {GroupKey::cat(), GroupKey::name()}),
                    specs());
            };
            auto coarse = [&] {
                return scan::agg(scan::group_by(scan::from_file(rgz, ridx),
                                                {GroupKey::cat()}),
                                 specs());
            };
            dataframe::DataFrame expect = collect_groupmap_plan(coarse());
            scan::run(fine()).get();
            check_match(expect, collect_engine_plan(coarse()), "cat");
        }

        // Occupancy (busy/active) must stay exact through the rollup: the
        // per-group +1/-1 delta-map serializes, and coarsening merges
        // delta-maps key-wise so the interval union and peak depth match a
        // fresh scan.
        SUBCASE("subsuming rollup: occupancy exact same-grain and coarsening") {
            TestEnvironment renv(200);
            REQUIRE(renv.is_valid());
            const std::string rgz = write_agg_engine_trace(renv.get_dir());
            const std::string ridx = determine_index_path(rgz, "");
            auto occ = [&](std::vector<GroupKey> gb) {
                return scan::agg(
                    scan::group_by(scan::from_file(rgz, ridx), std::move(gb)),
                    {
                        {AggOp::Busy, "", "busy"},
                        {AggOp::Active, "", "active"},
                    });
            };
            dataframe::DataFrame same_expect =
                collect_groupmap_plan(occ({GroupKey::cat()}));
            scan::run(occ({GroupKey::cat()})).get();
            check_match(same_expect,
                        collect_engine_plan(occ({GroupKey::cat()})), "cat");

            dataframe::DataFrame coarse_expect =
                collect_groupmap_plan(occ({GroupKey::cat()}));
            scan::run(occ({GroupKey::cat(), GroupKey::name()})).get();
            check_match(coarse_expect,
                        collect_engine_plan(occ({GroupKey::cat()})), "cat");
        }

        // Hist (a DDSketch list<struct> per group) must survive the rollup and
        // coarsening bin for bin.
        SUBCASE("subsuming rollup: Hist same-grain and coarsening") {
            TestEnvironment renv(200);
            REQUIRE(renv.is_valid());
            const std::string rgz = write_agg_engine_trace(renv.get_dir());
            const std::string ridx = determine_index_path(rgz, "");
            auto check_hist = [&](const dataframe::DataFrame& expect,
                                  const dataframe::DataFrame& got) {
                REQUIRE(expect.num_rows() == got.num_rows());
                for (std::int64_t lr = 0; lr < expect.num_rows(); ++lr) {
                    const std::string cat = bstr(expect, lr, "cat");
                    std::int64_t er = -1;
                    for (std::int64_t r = 0; r < got.num_rows(); ++r)
                        if (bstr(got, r, "cat") == cat) {
                            er = r;
                            break;
                        }
                    REQUIRE(er >= 0);
                    const auto lb = hist_bins(expect, lr, "h");
                    const auto eb = hist_bins(got, er, "h");
                    REQUIRE(lb.size() == eb.size());
                    for (std::size_t i = 0; i < lb.size(); ++i) {
                        CHECK(lb[i].lower == doctest::Approx(eb[i].lower));
                        CHECK(lb[i].upper == doctest::Approx(eb[i].upper));
                        CHECK(lb[i].count == eb[i].count);
                    }
                }
            };
            auto hv = [&](std::vector<GroupKey> gb) {
                return scan::agg(
                    scan::group_by(scan::from_file(rgz, ridx), std::move(gb)),
                    {{AggOp::Count, "", "n"}, {AggOp::Hist, "dur", "h"}});
            };
            dataframe::DataFrame same_expect =
                collect_groupmap_plan(hv({GroupKey::cat()}));
            scan::run(hv({GroupKey::cat()})).get();
            check_hist(same_expect, collect_engine_plan(hv({GroupKey::cat()})));

            dataframe::DataFrame coarse_expect =
                collect_groupmap_plan(hv({GroupKey::cat()}));
            scan::run(hv({GroupKey::cat(), GroupKey::name()})).get();
            check_hist(coarse_expect,
                       collect_engine_plan(hv({GroupKey::cat()})));
        }
    }
}
