#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_source.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "test_view_common.h"

namespace {

// A multi-member trace whose members carry different arg keys (x on the
// first half, y on the second), so a per-batch morsel's columns differ and
// the streaming path must exercise name-based reconciliation.
std::string create_mixed_arg_trace(TestEnvironment& env) {
    std::string pfw = env.get_dir() + "/mixed_args.pfw";
    std::ofstream ofs(pfw);
    for (int i = 0; i < 25; ++i)
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":)" << (10 + i)
            << R"(,"args":{"x":)" << i << R"(}})" << "\n";
    for (int i = 0; i < 25; ++i)
        ofs << R"({"ph":"X","name":"write","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (4000 + i * 100) << R"(,"dur":)" << (10 + i)
            << R"(,"args":{"y":)" << i << R"(}})" << "\n";
    ofs.close();
    std::string gz = pfw + ".gz";
    // A small member size forces the x-events and y-events into separate
    // gzip members, so the scan splits them into separate batches/morsels.
    dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, 512);
    fs::remove(pfw);
    return gz;
}

// (name, x, y) tuples, sorted for order-independent comparison across the
// streaming (fan-out order) and buffered (accumulate-then-build) paths.
std::vector<std::tuple<std::string, double, double>> row_key_set(
    const dataframe::DataFrame& df) {
    std::vector<std::tuple<std::string, double, double>> rows;
    const bool has_x = bhas(df, "args.x");
    const bool has_y = bhas(df, "args.y");
    for (std::int64_t i = 0; i < df.num_rows(); ++i) {
        const double x =
            has_x && !df.columns[static_cast<std::size_t>(bcol(df, "args.x"))]
                          .is_null(i)
                ? bnum(df, i, "args.x")
                : -1.0;
        const double y =
            has_y && !df.columns[static_cast<std::size_t>(bcol(df, "args.y"))]
                          .is_null(i)
                ? bnum(df, i, "args.y")
                : -1.0;
        rows.emplace_back(bstr(df, i, "name"), x, y);
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

}  // namespace

namespace scan = dftracer::utils::trace::views::detail::scan;

TEST_SUITE("trace scan - streaming row query") {
    TEST_CASE(
        "collect() streaming row query matches the buffered collect_frame()") {
        TestEnvironment env(200);
        std::string gz = create_mixed_arg_trace(env);
        std::string idx = determine_index_path(gz, "");
        scan::ScanPlan v = scan::metadata(scan::from_file(gz, idx), false);

        dataframe::DataFrame via_lazy = run(scan::collect(v).collect());
        dataframe::DataFrame via_eager = run(scan::collect_frame(v));

        REQUIRE(via_lazy.num_rows() == 50);
        REQUIRE(via_lazy.num_rows() == via_eager.num_rows());
        CHECK(bhas(via_lazy, "args.x"));
        CHECK(bhas(via_lazy, "args.y"));
        CHECK(bhas(via_eager, "args.x"));
        CHECK(bhas(via_eager, "args.y"));

        CHECK(row_key_set(via_lazy) == row_key_set(via_eager));
    }

    TEST_CASE("scan::stream() drained matches scan::collect()") {
        TestEnvironment env(200);
        std::string gz = create_mixed_arg_trace(env);
        std::string idx = determine_index_path(gz, "");
        scan::ScanPlan v = scan::metadata(scan::from_file(gz, idx), false);

        auto drain = [](const scan::ScanPlan& view) {
            return dftracer::utils::default_runtime()
                .submit([](scan::ScanPlan vv)
                            -> coro::CoroTask<std::vector<
                                std::tuple<std::string, double, double>>> {
                    std::vector<std::tuple<std::string, double, double>> rows;
                    auto gen = scan::stream(vv);
                    while (auto chunk = co_await gen.next()) {
                        auto part = row_key_set(*chunk);
                        rows.insert(rows.end(), part.begin(), part.end());
                    }
                    co_return rows;
                }(view))
                .get();
        };

        std::vector<std::tuple<std::string, double, double>> via_stream =
            drain(v);
        std::sort(via_stream.begin(), via_stream.end());
        dataframe::DataFrame via_collect = run(scan::collect(v).collect());

        REQUIRE(static_cast<std::int64_t>(via_stream.size()) ==
                via_collect.num_rows());
        CHECK(via_stream == row_key_set(via_collect));
    }

    TEST_CASE("collect() falls back to buffered collect_frame() with select") {
        const auto& s = shared_trace();
        scan::ScanPlan v =
            scan::select(scan::metadata(scan::from_file(s.gz, s.idx), false),
                         {"cat", "name"});

        dataframe::DataFrame via_lazy = run(scan::collect(v).collect());
        dataframe::DataFrame via_eager = run(scan::collect_frame(v));

        REQUIRE(via_lazy.num_rows() == via_eager.num_rows());
        CHECK(via_lazy.columns.size() == 2);
        CHECK(row_key_set(via_lazy) == row_key_set(via_eager));
    }

    TEST_CASE(
        "scan::stream() bounds a buffered select-plan into multiple chunks") {
        const auto& s = shared_trace();  // 50 rows
        scan::ScanPlan v =
            scan::select(scan::metadata(scan::from_file(s.gz, s.idx), false),
                         {"cat", "name"});

        auto [chunks, rows] =
            dftracer::utils::default_runtime()
                .submit([](scan::ScanPlan vv)
                            -> coro::CoroTask<
                                std::pair<std::size_t, std::int64_t>> {
                    std::size_t n = 0;
                    std::int64_t nrows = 0;
                    auto gen = scan::stream(vv, 10);
                    while (auto chunk = co_await gen.next()) {
                        ++n;
                        nrows += chunk->num_rows();
                    }
                    co_return std::make_pair(n, nrows);
                }(v))
                .get();

        dataframe::DataFrame via_collect = run(scan::collect(v).collect());
        CHECK(chunks > 1);
        CHECK(rows == via_collect.num_rows());
    }

    TEST_CASE(
        "scan::stream() with select+sort+limit streams multiple bounded "
        "chunks matching collect()") {
        const auto& s = shared_trace();  // 50 rows
        scan::ScanPlan v = scan::limit(
            scan::sort_by(scan::select(scan::metadata(
                                           scan::from_file(s.gz, s.idx), false),
                                       {"cat", "name", "dur"}),
                          "dur", /*descending=*/true),
            20);

        auto [chunks, rows] =
            dftracer::utils::default_runtime()
                .submit([](scan::ScanPlan vv)
                            -> coro::CoroTask<
                                std::pair<std::size_t, std::int64_t>> {
                    std::size_t n = 0;
                    std::int64_t nrows = 0;
                    auto gen = scan::stream(vv, 5);
                    while (auto chunk = co_await gen.next()) {
                        ++n;
                        nrows += chunk->num_rows();
                    }
                    co_return std::make_pair(n, nrows);
                }(v))
                .get();

        dataframe::DataFrame via_collect = run(scan::collect(v).collect());
        CHECK(chunks > 1);
        CHECK(rows == via_collect.num_rows());
        REQUIRE(via_collect.num_rows() == 20);
        REQUIRE(via_collect.columns.size() == 3);
        // sort_by(dur, desc) then limit(20): the top 20 durations, descending.
        for (std::int64_t i = 1; i < via_collect.num_rows(); ++i)
            CHECK(bnum(via_collect, i - 1, "dur") >=
                  bnum(via_collect, i, "dur"));
    }

    TEST_CASE(
        "aggregated trace scan sort_by+limit is identical whether run via "
        "collect() or collect_frame()") {
        TestEnvironment env(200);
        std::string gz = create_mixed_arg_trace(env);
        std::string idx = determine_index_path(gz, "");
        scan::ScanPlan v = scan::limit(
            scan::sort_by(scan::agg(scan::group_by(scan::from_file(gz, idx),
                                                   {GroupKey::name()}),
                                    {{AggOp::Count, "", "n"}}),
                          "n", /*descending=*/true),
            1);

        dataframe::DataFrame via_lazy = run(scan::collect(v).collect());
        dataframe::DataFrame via_eager = run(scan::collect_frame(v));

        REQUIRE(via_lazy.num_rows() == 1);
        REQUIRE(via_lazy.num_rows() == via_eager.num_rows());
        // n is the deterministic max count; the winning name can tie, and
        // limit(1) then picks one arbitrarily, so the two paths may keep a
        // different max-count name. Assert the count, not the tied name.
        CHECK(bnum(via_lazy, 0, "n") == bnum(via_eager, 0, "n"));
    }

    TEST_CASE("ViewSource pushdown: col filter -> Query (Exact), matches") {
        namespace df = dftracer::utils::dataframe;
        const auto& s = shared_trace();
        scan::ScanPlan v = scan::metadata(scan::from_file(s.gz, s.idx), false);
        auto src = std::make_shared<ViewSource>(v);

        const std::vector<std::string> names = src->names();
        int di = -1;
        for (int i = 0; i < static_cast<int>(names.size()); ++i)
            if (names[i] == "dur") di = i;
        REQUIRE(di >= 0);

        // Direct scan(): the predicate is translatable and fully applied by the
        // trace scan, so it comes back Exact.
        df::ScanRequest req;
        req.filters.push_back(df::col(di) > std::int64_t{25});
        df::ScanResult sr = src->scan(req);
        REQUIRE(sr.filters.size() == 1);
        CHECK(sr.filters[0] == df::Pushed::Exact);

        // End-to-end: the pushed-down result equals engine-applied filtering
        // over the unfiltered events.
        df::DataFrame filtered = run(df::LazyFrame::scan(src)
                                         .filter(df::col(di) > std::int64_t{25})
                                         .collect());
        df::DataFrame all = run(scan::collect(v).collect());
        std::int64_t expect = 0;
        for (std::int64_t i = 0; i < all.num_rows(); ++i)
            if (bnum(all, i, "dur") > 25) ++expect;
        CHECK(expect > 0);
        CHECK(filtered.num_rows() == expect);
        for (std::int64_t i = 0; i < filtered.num_rows(); ++i)
            CHECK(bnum(filtered, i, "dur") > 25);
    }

    TEST_CASE("ViewSource pushdown: non-translatable filter falls back (No)") {
        namespace df = dftracer::utils::dataframe;
        const auto& s = shared_trace();
        scan::ScanPlan v = scan::metadata(scan::from_file(s.gz, s.idx), false);
        auto src = std::make_shared<ViewSource>(v);

        const std::vector<std::string> names = src->names();
        int di = -1;
        for (int i = 0; i < static_cast<int>(names.size()); ++i)
            if (names[i] == "dur") di = i;
        REQUIRE(di >= 0);

        // (dur + dur) > 50 is not a bare col-cmp-scalar, so it cannot be
        // translated to a Query; the engine applies it instead.
        df::Expr pred = (df::col(di) + df::col(di)) > std::int64_t{50};
        df::ScanRequest req;
        req.filters.push_back(pred);
        df::ScanResult sr = src->scan(req);
        REQUIRE(sr.filters.size() == 1);
        CHECK(sr.filters[0] == df::Pushed::No);

        df::DataFrame got =
            run(df::LazyFrame::scan(src).filter(pred).collect());
        df::DataFrame all = run(scan::collect(v).collect());
        std::int64_t expect = 0;
        for (std::int64_t i = 0; i < all.num_rows(); ++i)
            if (2 * bnum(all, i, "dur") > 50) ++expect;
        CHECK(expect > 0);
        CHECK(got.num_rows() == expect);
        for (std::int64_t i = 0; i < got.num_rows(); ++i)
            CHECK(2 * bnum(got, i, "dur") > 50);
    }

    TEST_CASE("ViewSource pushdown: string predicates and is_in") {
        namespace df = dftracer::utils::dataframe;
        const auto& s = shared_trace();  // 30 "read" + 20 "fwrite"
        scan::ScanPlan v = scan::metadata(scan::from_file(s.gz, s.idx), false);
        auto src = std::make_shared<ViewSource>(v);

        const std::vector<std::string> names = src->names();
        int ni = -1;
        for (int i = 0; i < static_cast<int>(names.size()); ++i)
            if (names[i] == "name") ni = i;
        REQUIRE(ni >= 0);

        auto pushed_as = [&](const df::Expr& pred) {
            df::ScanRequest req;
            req.filters.push_back(pred);
            df::ScanResult sr = src->scan(req);
            REQUIRE(sr.filters.size() == 1);
            return sr.filters[0];
        };
        auto scanned = [&](const df::Expr& pred) {
            df::DataFrame out =
                run(df::LazyFrame::scan(src).filter(pred).collect());
            std::map<std::string, std::int64_t> by_name;
            for (std::int64_t i = 0; i < out.num_rows(); ++i)
                ++by_name[bstr(out, i, "name")];
            return by_name;
        };
        using M = std::map<std::string, std::int64_t>;

        SUBCASE("like pushes exactly") {
            df::Expr pred =
                df::expr_str_pred(df::StrPredOp::Like, df::col(ni), "fw%");
            CHECK(pushed_as(pred) == df::Pushed::Exact);
            CHECK(scanned(pred) == M{{"fwrite", 20}});
        }
        SUBCASE("starts_with and ends_with push as an escaped like") {
            df::Expr pre =
                df::expr_str_pred(df::StrPredOp::StartsWith, df::col(ni), "re");
            CHECK(pushed_as(pre) == df::Pushed::Exact);
            CHECK(scanned(pre) == M{{"read", 30}});
            df::Expr suf =
                df::expr_str_pred(df::StrPredOp::EndsWith, df::col(ni), "ite");
            CHECK(pushed_as(suf) == df::Pushed::Exact);
            CHECK(scanned(suf) == M{{"fwrite", 20}});
            // A wildcard in the literal is escaped, so it matches nothing here
            // rather than everything.
            df::Expr lit =
                df::expr_str_pred(df::StrPredOp::StartsWith, df::col(ni), "%");
            CHECK(pushed_as(lit) == df::Pushed::Exact);
            CHECK(scanned(lit).empty());
        }
        SUBCASE("contains pushes as a case-insensitive superset, Inexact") {
            df::Expr pred =
                df::expr_str_pred(df::StrPredOp::Contains, df::col(ni), "ea");
            CHECK(pushed_as(pred) == df::Pushed::Inexact);
            CHECK(scanned(pred) == M{{"read", 30}});
            // The engine re-applies the case-sensitive form over the superset.
            df::Expr upper =
                df::expr_str_pred(df::StrPredOp::Contains, df::col(ni), "EA");
            CHECK(pushed_as(upper) == df::Pushed::Inexact);
            CHECK(scanned(upper).empty());
        }
        SUBCASE("regex stays with the engine") {
            df::Expr pred =
                df::expr_str_pred(df::StrPredOp::Search, df::col(ni), "^f");
            CHECK(pushed_as(pred) == df::Pushed::No);
            CHECK(scanned(pred) == M{{"fwrite", 20}});
        }
        SUBCASE("is_in pushes exactly") {
            df::Expr pred =
                df::expr_is_in(df::col(ni), df::Series::strings({"read", "x"}));
            CHECK(pushed_as(pred) == df::Pushed::Exact);
            CHECK(scanned(pred) == M{{"read", 30}});
            CHECK(pushed_as(~pred) == df::Pushed::Exact);
            CHECK(scanned(~pred) == M{{"fwrite", 20}});
        }
    }

    TEST_CASE("ViewSource pushdown: AND/OR/NOT of col filters") {
        namespace df = dftracer::utils::dataframe;
        const auto& s = shared_trace();
        scan::ScanPlan v = scan::metadata(scan::from_file(s.gz, s.idx), false);
        auto src = std::make_shared<ViewSource>(v);

        const std::vector<std::string> names = src->names();
        int di = -1, ti = -1;
        for (int i = 0; i < static_cast<int>(names.size()); ++i) {
            if (names[i] == "dur") di = i;
            if (names[i] == "ts") ti = i;
        }
        REQUIRE(di >= 0);
        REQUIRE(ti >= 0);

        df::DataFrame all = run(scan::collect(v).collect());

        auto pushed_as = [&](const df::Expr& pred) {
            df::ScanRequest req;
            req.filters.push_back(pred);
            df::ScanResult sr = src->scan(req);
            REQUIRE(sr.filters.size() == 1);
            return sr.filters[0];
        };
        auto rows_matching = [&](auto keep) {
            std::int64_t n = 0;
            for (std::int64_t i = 0; i < all.num_rows(); ++i)
                if (keep(bnum(all, i, "dur"), bnum(all, i, "ts"))) ++n;
            return n;
        };
        auto scanned = [&](const df::Expr& pred) {
            return run(df::LazyFrame::scan(src).filter(pred).collect())
                .num_rows();
        };

        SUBCASE("both conjuncts translate, so the AND is Exact") {
            df::Expr pred = (df::col(di) > std::int64_t{25}) &
                            (df::col(di) < std::int64_t{45});
            CHECK(pushed_as(pred) == df::Pushed::Exact);
            const std::int64_t want = rows_matching(
                [](double d, double) { return d > 25 && d < 45; });
            CHECK(want > 0);
            CHECK(scanned(pred) == want);
        }

        SUBCASE(
            "one untranslatable conjunct leaves the other pushed, Inexact") {
            // The pushed half selects a superset, so the engine must re-apply
            // the predicate; marking this Exact would return the superset.
            df::Expr pred = (df::col(di) > std::int64_t{25}) &
                            ((df::col(di) + df::col(di)) > std::int64_t{70});
            CHECK(pushed_as(pred) == df::Pushed::Inexact);
            const std::int64_t want = rows_matching(
                [](double d, double) { return d > 25 && 2 * d > 70; });
            CHECK(want > 0);
            CHECK(scanned(pred) == want);
        }

        SUBCASE("both disjuncts translate, so the OR is Exact") {
            df::Expr pred = (df::col(di) < std::int64_t{15}) |
                            (df::col(di) > std::int64_t{45});
            CHECK(pushed_as(pred) == df::Pushed::Exact);
            const std::int64_t want = rows_matching(
                [](double d, double) { return d < 15 || d > 45; });
            CHECK(want > 0);
            CHECK(scanned(pred) == want);
        }

        SUBCASE("one untranslatable disjunct sinks the whole OR") {
            // Pushing only the translatable side would drop every row the
            // other side keeps, so nothing is pushed at all.
            df::Expr pred = (df::col(di) > std::int64_t{45}) |
                            ((df::col(di) + df::col(di)) < std::int64_t{30});
            CHECK(pushed_as(pred) == df::Pushed::No);
            const std::int64_t want = rows_matching(
                [](double d, double) { return d > 45 || 2 * d < 30; });
            CHECK(want > 0);
            CHECK(scanned(pred) == want);
        }

        SUBCASE("NOT of an exact predicate is Exact") {
            df::Expr pred = ~(df::col(di) > std::int64_t{25});
            CHECK(pushed_as(pred) == df::Pushed::Exact);
            // Regression: the chunk pruner used to answer a NotNode with the
            // set-difference complement of its operand's may-match chunks,
            // which dropped every chunk holding events on both sides of the
            // comparison. This came back empty.
            const std::int64_t want =
                rows_matching([](double d, double) { return !(d > 25); });
            CHECK(want > 0);
            CHECK(scanned(pred) == want);
        }

        SUBCASE("NOT of a merely-superset predicate is not pushed") {
            // Negating a superset yields a SUBSET, which would drop rows the
            // predicate keeps, so the whole NOT falls back to the engine.
            df::Expr pred = ~((df::col(di) > std::int64_t{25}) &
                              ((df::col(di) + df::col(di)) > std::int64_t{70}));
            CHECK(pushed_as(pred) == df::Pushed::No);
            const std::int64_t want = rows_matching(
                [](double d, double) { return !(d > 25 && 2 * d > 70); });
            CHECK(want > 0);
            CHECK(scanned(pred) == want);
        }

        SUBCASE("two columns combine") {
            df::Expr pred = (df::col(di) > std::int64_t{25}) &
                            (df::col(ti) > std::int64_t{0});
            CHECK(pushed_as(pred) == df::Pushed::Exact);
            const std::int64_t want = rows_matching(
                [](double d, double t) { return d > 25 && t > 0; });
            CHECK(want > 0);
            CHECK(scanned(pred) == want);
        }
    }

    TEST_CASE(
        "ViewSource pushdown: projection harvests only selected columns") {
        namespace df = dftracer::utils::dataframe;
        const auto& s = shared_trace();
        scan::ScanPlan v = scan::metadata(scan::from_file(s.gz, s.idx), false);
        auto src = std::make_shared<ViewSource>(v);

        df::DataFrame proj =
            run(df::LazyFrame::scan(src).select({"cat", "name"}).collect());
        REQUIRE(proj.columns.size() == 2);
        CHECK(bhas(proj, "cat"));
        CHECK(bhas(proj, "name"));
        CHECK(proj.num_rows() == 50);
        for (std::int64_t i = 0; i < proj.num_rows(); ++i) {
            const std::string cat = bstr(proj, i, "cat");
            CHECK((cat == "POSIX" || cat == "STDIO"));
        }
    }

    TEST_CASE("ViewSource head(10) early-stops the scan (cancellation)") {
        namespace df = dftracer::utils::dataframe;
        const auto& s = shared_trace();  // 50 events
        scan::ScanPlan v = scan::metadata(scan::from_file(s.gz, s.idx), false);
        auto src = std::make_shared<ViewSource>(v);

        df::DataFrame h = run(df::LazyFrame::scan(src).head(10).collect());
        CHECK(h.num_rows() == 10);
    }

    TEST_CASE(
        "ViewSource schema() types match the actually collected columns "
        "for a statically-known select") {
        namespace df = dftracer::utils::dataframe;
        const auto& s = shared_trace();
        scan::ScanPlan v =
            scan::select(scan::metadata(scan::from_file(s.gz, s.idx), false),
                         {"name", "cat", "pid", "ts", "dur"});
        auto src = std::make_shared<ViewSource>(v);

        df::Schema schema = src->schema();
        REQUIRE(schema.fields.size() == 5);

        df::DataFrame got = run(df::LazyFrame::scan(src).collect());
        REQUIRE(got.columns.size() == schema.fields.size());
        for (std::size_t i = 0; i < schema.fields.size(); ++i) {
            const std::int64_t ci = bcol(got, schema.fields[i].name);
            REQUIRE(ci >= 0);
            CHECK(schema.fields[i].type.id ==
                  got.columns[static_cast<std::size_t>(ci)].type());
        }
    }

    TEST_CASE(
        "ViewSource schema() reports every column, marking a data-dependent "
        "args.* value Unknown") {
        namespace df = dftracer::utils::dataframe;
        TestEnvironment env(200);
        std::string gz = create_mixed_arg_trace(env);
        std::string idx = determine_index_path(gz, "");
        scan::ScanPlan v =
            scan::select(scan::metadata(scan::from_file(gz, idx), false),
                         {"name", "args.x"});
        auto src = std::make_shared<ViewSource>(v);

        df::Schema schema = src->schema();
        REQUIRE(schema.fields.size() == 2);
        CHECK(schema.fields[0].name == "name");
        CHECK(schema.fields[1].name == "args.x");
        // A column whose type the scan infers per batch says so rather than
        // silencing the whole schema.
        CHECK(schema.fields[0].type.id == df::TypeId::String);
        CHECK(schema.fields[1].type.id == df::TypeId::Unknown);

        // schema() may report Unknown for a data-dependent column, but the
        // collected frame always resolves to a concrete type.
        df::DataFrame got = run(df::LazyFrame::scan(src).collect());
        for (const df::Series& c : got.columns)
            CHECK(c.type() != df::TypeId::Unknown);
    }

    TEST_CASE(
        "ViewSource schema() types match the buffered frame for an "
        "aggregated view") {
        namespace df = dftracer::utils::dataframe;
        TestEnvironment env(200);
        std::string gz = create_mixed_arg_trace(env);
        std::string idx = determine_index_path(gz, "");
        scan::ScanPlan v = scan::agg(
            scan::group_by(scan::from_file(gz, idx), {GroupKey::name()}),
            {{AggOp::Count, "", "n"}});
        auto src = std::make_shared<ViewSource>(v);

        df::Schema schema = src->schema();
        df::DataFrame buf = run(scan::collect_frame(v));
        REQUIRE(schema.fields.size() == buf.names.size());
        for (std::size_t i = 0; i < buf.columns.size(); ++i) {
            CHECK(schema.fields[i].name == buf.names[i]);
            CHECK(schema.fields[i].type.id == buf.columns[i].type());
        }
    }

    TEST_CASE("scan::stream() over a histogram aggregation yields one chunk") {
        TestEnvironment env(200);
        std::string gz = create_mixed_arg_trace(env);
        std::string idx = determine_index_path(gz, "");
        scan::ScanPlan v = scan::agg(
            scan::group_by(scan::from_file(gz, idx), {GroupKey::cat()}),
            {{AggOp::Hist, "dur", "h"}});

        std::size_t chunks =
            dftracer::utils::default_runtime()
                .submit([](scan::ScanPlan vv) -> coro::CoroTask<std::size_t> {
                    std::size_t n = 0;
                    auto gen = scan::stream(vv, 1);
                    while (auto chunk = co_await gen.next()) ++n;
                    co_return n;
                }(v))
                .get();

        CHECK(chunks == 1);
    }

    TEST_CASE(
        "ViewSource schema() reports a concrete type for an args column once "
        "the index has harvested it") {
        namespace df = dftracer::utils::dataframe;
        TestEnvironment env(200);
        std::string gz = create_mixed_arg_trace(env);
        std::string idx = determine_index_path(gz, "");

        // Build the index (and its harvested column types) before asking for
        // the schema, so the "x" arg's type is known from the index rather
        // than data-dependent.
        {
            StringSink sink;
            scan::export_json(
                scan::emit_all_metadata(scan::from_file(gz, idx), true), sink)
                .get();
        }

        scan::ScanPlan v =
            scan::select(scan::metadata(scan::from_file(gz, idx), false),
                         {"name", "args.x"});
        auto src = std::make_shared<ViewSource>(v);

        df::Schema schema = src->schema();
        REQUIRE(schema.fields.size() == 2);
        CHECK(schema.fields[0].name == "name");
        CHECK(schema.fields[1].name == "args.x");
        CHECK(schema.fields[0].type.id == df::TypeId::String);
        // The index harvested "x" as an integer column; schema() must report
        // that concretely instead of falling back to Unknown.
        CHECK(schema.fields[1].type.id == df::TypeId::Int64);
    }
}
