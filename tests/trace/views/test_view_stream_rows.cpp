// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/tuple.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/trace/views/view_source.h>
#include <doctest/doctest.h>
#include <generator_ok.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <fstream>
#include <memory>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

namespace {

// A multi-member trace whose members carry different arg keys (x on the
// first half, y on the second), so a per-batch morsel's columns differ and
// the streaming path must exercise name-based reconciliation.
dstd::String create_mixed_arg_trace(TestEnvironment& env) {
    dstd::String pfw = env.dir() + "/mixed_args.pfw";
    dftu_utils_test::FileOut ofs(pfw);
    for (int i = 0; i < 25; ++i)
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":)" << (10 + i)
            << R"(,"args":{"x":)" << i << R"(}})" << "\n";
    for (int i = 0; i < 25; ++i)
        ofs << R"({"ph":"X","name":"write","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (4000 + i * 100) << R"(,"dur":)" << (10 + i)
            << R"(,"args":{"y":)" << i << R"(}})" << "\n";
    ofs.close();
    dstd::String gz = pfw + ".gz";
    // A small member size forces the x-events and y-events into separate
    // gzip members, so the scan splits them into separate batches/morsels.
    dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, 512);
    dstd::fs::remove(pfw).discard();
    return gz;
}

// (name, x, y) tuples, sorted for order-independent comparison across the
// streaming (fan-out order) and buffered (accumulate-then-build) paths.
dstd::Vector<dstd::Tuple<dstd::String, double, double>> row_key_set(
    const dataframe::DataFrame& df) {
    dstd::Vector<dstd::Tuple<dstd::String, double, double>> rows;
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

// 40 events whose args carry exactly `keys`, in a trace named `name`.
dstd::String create_args_trace(TestEnvironment& env, const dstd::String& name,
                               const dstd::Vector<dstd::String>& keys) {
    const dstd::String pfw = env.dir() + "/" + name + ".pfw";
    dftu_utils_test::FileOut ofs(pfw);
    for (int i = 0; i < 40; ++i) {
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":)" << (10 + i) << R"(,"args":{)";
        for (std::size_t k = 0; k < keys.size(); ++k)
            ofs << (k ? "," : "") << '"' << keys[k] << R"(":)" << i;
        ofs << "}}\n";
    }
    ofs.close();
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, 512);
    dstd::fs::remove(pfw).has_value();
    return gz;
}

dstd::Set<dstd::String> arg_columns(const dataframe::DataFrame& df) {
    dstd::Set<dstd::String> out;
    for (const dstd::String& n : df.names)
        if (n.rfind("args.", 0) == 0) out.insert(dstd::String(n));
    return out;
}

}  // namespace

using namespace dftracer::utils::trace;

namespace {

constexpr int DISTINCT_EVENTS = 900;

dstd::String distinct_run(int i) {
    const auto u = static_cast<unsigned>(i);
    return "r" + dstd::to_string((u * u) % 17);
}
dstd::String distinct_metric(int i) {
    return "m" + dstd::to_string((i / 7 + i % 5) % 9);
}

// 900 events whose (run, metric) pairs first appear at scattered rows, written
// into many small gzip members and indexed with a tiny checkpoint size, so the
// scan has many units and first occurrences land in different ones.
dstd::String create_distinct_trace(TestEnvironment& env,
                                   const dstd::String& idx_dir) {
    dstd::String pfw = env.dir() + "/distinct.pfw";
    dftu_utils_test::FileOut ofs(pfw);
    for (int i = 0; i < DISTINCT_EVENTS; ++i) {
        static const char* const NAMES[] = {"read", "write", "open", "lseek"};
        ofs << R"({"ph":"X","name":")" << NAMES[i % 4]
            << R"(","cat":"POSIX","pid":1,"tid":1,"ts":)" << (1000 + i)
            << R"(,"dur":1,"run":")" << distinct_run(i) << R"(","metric":")"
            << distinct_metric(i) << R"(","val":)" << (i % 5) << "}\n";
    }
    ofs.close();
    dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, 512);
    dstd::fs::remove(pfw).has_value();
    REQUIRE(dftu_utils_test::build_index(gz, idx_dir,
                                         /*checkpoint_size=*/256));
    return gz;
}

}  // namespace

TEST_SUITE("trace scan - streaming row query") {
    TEST_CASE("a select-all frame holds only its own trace's args columns") {
        TestEnvironment env(200);
        const dstd::String gz_a = create_args_trace(env, "trace_a", {"a", "b"});
        const dstd::String gz_b = create_args_trace(env, "trace_b", {"c"});
        const dstd::Set<dstd::String> only_a{"args.a", "args.b"};
        const dstd::Set<dstd::String> only_b{"args.c"};

        const auto plan_a = views::detail::scan::from_file(
            dstd::String(gz_a),
            dstd::String(dstd::String(determine_index_path(gz_a, ""))));
        CHECK(arg_columns(run(
                  views::detail::scan::collect(plan_a).collect())) == only_a);

        const auto plan_b = views::detail::scan::from_file(
            dstd::String(gz_b),
            dstd::String(dstd::String(determine_index_path(gz_b, ""))));
        CHECK(arg_columns(run(
                  views::detail::scan::collect(plan_b).collect())) == only_b);
        CHECK(arg_columns(run(views::detail::scan::collect_frame(plan_b))) ==
              only_b);

        // The schema a plan declares comes from the index it reads, which
        // here holds both traces.
        dstd::Vector<dstd::String> schema_b;
        for (const auto& name :
             dftu_utils_test::ok(views::detail::scan::columns(plan_b)))
            schema_b.emplace_back(name);
        CHECK(std::find(schema_b.begin(), schema_b.end(), "a") ==
              schema_b.end());
        CHECK(std::find(schema_b.begin(), schema_b.end(), "c") !=
              schema_b.end());
        dataframe::Schema declared = ViewSource(plan_b).schema();
        dstd::Set<dstd::String> declared_args;
        for (const auto& f : declared.fields)
            if (f.name.rfind("args.", 0) == 0)
                declared_args.insert(dstd::String(f.name));
        CHECK(declared_args == only_b);
    }

    TEST_CASE("traces sharing one index root keep their own args columns") {
        TestEnvironment env(200);
        const dstd::String gz_a = create_args_trace(env, "trace_a", {"a", "b"});
        const dstd::String gz_b = create_args_trace(env, "trace_b", {"c"});
        const dstd::String idx_dir = env.dir() + "/shared.dftindex";
        REQUIRE(dftu_utils_test::build_index(gz_a, idx_dir, 256));
        REQUIRE(dftu_utils_test::build_index(gz_b, idx_dir, 256));

        const auto plan_a = views::detail::scan::from_file(
            dstd::String(gz_a),
            dstd::String(dstd::String(determine_index_path(gz_a, idx_dir))));
        CHECK(
            arg_columns(run(views::detail::scan::collect(plan_a).collect())) ==
            dstd::Set<dstd::String>{"args.a", "args.b"});
        const auto plan_b = views::detail::scan::from_file(
            dstd::String(gz_b),
            dstd::String(dstd::String(determine_index_path(gz_b, idx_dir))));
        CHECK(
            arg_columns(run(views::detail::scan::collect(plan_b).collect())) ==
            dstd::Set<dstd::String>{"args.c"});
        CHECK(arg_columns(run(views::detail::scan::collect_frame(plan_b))) ==
              dstd::Set<dstd::String>{"args.c"});
    }

    TEST_CASE(
        "collect() streaming row query matches the buffered collect_frame()") {
        TestEnvironment env(200);
        dstd::String gz = create_mixed_arg_trace(env);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));
        views::detail::scan::ScanPlan v =
            views::detail::scan::from_file(dstd::String(gz), dstd::String(idx));

        dataframe::DataFrame via_lazy =
            run(views::detail::scan::collect(v).collect());
        dataframe::DataFrame via_eager =
            run(views::detail::scan::collect_frame(v));

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
        dstd::String gz = create_mixed_arg_trace(env);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));
        views::detail::scan::ScanPlan v =
            views::detail::scan::from_file(dstd::String(gz), dstd::String(idx));

        auto drain = [](const views::detail::scan::ScanPlan& view) {
            return dftracer::utils::default_runtime()
                .submit([](views::detail::scan::ScanPlan vv)
                            -> coro::CoroTask<dstd::Vector<
                                dstd::Tuple<dstd::String, double, double>>> {
                    dstd::Vector<dstd::Tuple<dstd::String, double, double>>
                        rows;
                    auto gen = dftu_utils_test::ok_items(
                        views::detail::scan::stream(vv));
                    while (auto chunk = co_await gen.next()) {
                        auto part = row_key_set(*chunk);
                        rows.insert(rows.end(), part.begin(), part.end());
                    }
                    co_return rows;
                }(view))
                .get();
        };

        dstd::Vector<dstd::Tuple<dstd::String, double, double>> via_stream =
            dftu_utils_test::ok(drain(v));
        std::sort(via_stream.begin(), via_stream.end());
        dataframe::DataFrame via_collect =
            run(views::detail::scan::collect(v).collect());

        REQUIRE(static_cast<std::int64_t>(via_stream.size()) ==
                via_collect.num_rows());
        CHECK(via_stream == row_key_set(via_collect));
    }

    TEST_CASE("collect() falls back to buffered collect_frame() with select") {
        const auto& s = shared_trace();
        views::detail::scan::ScanPlan v = views::detail::scan::select(
            views::detail::scan::from_file(dstd::String(s.gz),
                                           dstd::String(s.idx)),
            {"cat", "name"});

        dataframe::DataFrame via_lazy =
            run(views::detail::scan::collect(v).collect());
        dataframe::DataFrame via_eager =
            run(views::detail::scan::collect_frame(v));

        REQUIRE(via_lazy.num_rows() == via_eager.num_rows());
        CHECK(via_lazy.columns.size() == 2);
        CHECK(row_key_set(via_lazy) == row_key_set(via_eager));
    }

    TEST_CASE(
        "scan::stream() bounds a buffered select-plan into multiple chunks") {
        const auto& s = shared_trace();  // 50 rows
        views::detail::scan::ScanPlan v = views::detail::scan::select(
            views::detail::scan::from_file(dstd::String(s.gz),
                                           dstd::String(s.idx)),
            {"cat", "name"});

        auto [chunks, rows] = dftu_utils_test::ok(
            dftracer::utils::default_runtime()
                .submit([](views::detail::scan::ScanPlan vv)
                            -> coro::CoroTask<
                                dstd::Pair<std::size_t, std::int64_t>> {
                    std::size_t n = 0;
                    std::int64_t nrows = 0;
                    auto gen = dftu_utils_test::ok_items(
                        views::detail::scan::stream(vv, 10));
                    while (auto chunk = co_await gen.next()) {
                        ++n;
                        nrows += chunk->num_rows();
                    }
                    co_return dstd::make_pair(n, nrows);
                }(v))
                .get());

        dataframe::DataFrame via_collect =
            run(views::detail::scan::collect(v).collect());
        CHECK(chunks > 1);
        CHECK(rows == via_collect.num_rows());
    }

    TEST_CASE(
        "scan::stream() with select+sort+limit streams multiple bounded "
        "chunks matching collect()") {
        const auto& s = shared_trace();  // 50 rows
        views::detail::scan::ScanPlan v = views::detail::scan::limit(
            views::detail::scan::sort_by(
                views::detail::scan::select(
                    views::detail::scan::from_file(dstd::String(s.gz),
                                                   dstd::String(s.idx)),
                    {"cat", "name", "dur"}),
                "dur", /*descending=*/true),
            20);

        auto [chunks, rows] = dftu_utils_test::ok(
            dftracer::utils::default_runtime()
                .submit([](views::detail::scan::ScanPlan vv)
                            -> coro::CoroTask<
                                dstd::Pair<std::size_t, std::int64_t>> {
                    std::size_t n = 0;
                    std::int64_t nrows = 0;
                    auto gen = dftu_utils_test::ok_items(
                        views::detail::scan::stream(vv, 5));
                    while (auto chunk = co_await gen.next()) {
                        ++n;
                        nrows += chunk->num_rows();
                    }
                    co_return dstd::make_pair(n, nrows);
                }(v))
                .get());

        dataframe::DataFrame via_collect =
            run(views::detail::scan::collect(v).collect());
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
        dstd::String gz = create_mixed_arg_trace(env);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));
        views::detail::scan::ScanPlan v = views::detail::scan::limit(
            views::detail::scan::sort_by(
                views::detail::scan::agg(
                    views::detail::scan::group_by(
                        views::detail::scan::from_file(dstd::String(gz),
                                                       dstd::String(idx)),
                        {GroupKey::name()}),
                    {{AggOp::Count, "", "n"}}),
                "n", /*descending=*/true),
            1);

        dataframe::DataFrame via_lazy =
            run(views::detail::scan::collect(v).collect());
        dataframe::DataFrame via_eager =
            run(views::detail::scan::collect_frame(v));

        REQUIRE(via_lazy.num_rows() == 1);
        REQUIRE(via_lazy.num_rows() == via_eager.num_rows());
        // n is the deterministic max count; the winning name can tie, and
        // limit(1) then picks one arbitrarily, so the two paths may keep a
        // different max-count name. Assert the count, not the tied name.
        CHECK(bnum(via_lazy, 0, "n") == bnum(via_eager, 0, "n"));
    }

    TEST_CASE("a ViewSource whose scan fails returns the error, not a throw") {
        TestEnvironment env(200);
        dstd::String gz = create_mixed_arg_trace(env);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));
        views::detail::scan::ScanPlan v = views::detail::scan::agg(
            views::detail::scan::group_by(
                views::detail::scan::from_file(dstd::String(gz),
                                               dstd::String(idx)),
                {GroupKey::name()}),
            {{AggOp::Count, "", "n"}});
        auto src = dstd::make_shared<ViewSource>(v);

        dataframe::ScanRequest req;
        req.projection = {"no_such_column"};
        const auto scanned = src->scan(req);
        REQUIRE_FALSE(scanned.has_value());
        CHECK(
            dstd::StringView(scanned.error().c_str()).find("no_such_column") !=
            dstd::StringView::npos);
    }

    TEST_CASE("ViewSource pushdown: col filter -> Query (Exact), matches") {
        const auto& s = shared_trace();
        views::detail::scan::ScanPlan v = views::detail::scan::from_file(
            dstd::String(s.gz), dstd::String(s.idx));
        auto src = dstd::make_shared<ViewSource>(v);

        const dstd::Vector<dstd::String> names = src->names();
        int di = -1;
        for (int i = 0; i < static_cast<int>(names.size()); ++i)
            if (names[i] == "dur") di = i;
        REQUIRE(di >= 0);

        // Direct scan(): the predicate is translatable and fully applied by the
        // trace scan, so it comes back Exact.
        dataframe::ScanRequest req;
        req.filters.push_back(dataframe::col(di) > std::int64_t{25});
        dataframe::ScanResult sr = dftu_utils_test::ok(src->scan(req));
        REQUIRE(sr.filters.size() == 1);
        CHECK(sr.filters[0] == dataframe::Pushed::Exact);

        // End-to-end: the pushed-down result equals engine-applied filtering
        // over the unfiltered events.
        dataframe::DataFrame filtered =
            run(dataframe::LazyFrame::scan(src)
                    .filter(dataframe::col(di) > std::int64_t{25})
                    .collect());
        dataframe::DataFrame all =
            run(views::detail::scan::collect(v).collect());
        std::int64_t expect = 0;
        for (std::int64_t i = 0; i < all.num_rows(); ++i)
            if (bnum(all, i, "dur") > 25) ++expect;
        CHECK(expect > 0);
        CHECK(filtered.num_rows() == expect);
        for (std::int64_t i = 0; i < filtered.num_rows(); ++i)
            CHECK(bnum(filtered, i, "dur") > 25);
    }

    TEST_CASE("ViewSource pushdown: non-translatable filter falls back (No)") {
        const auto& s = shared_trace();
        views::detail::scan::ScanPlan v = views::detail::scan::from_file(
            dstd::String(s.gz), dstd::String(s.idx));
        auto src = dstd::make_shared<ViewSource>(v);

        const dstd::Vector<dstd::String> names = src->names();
        int di = -1;
        for (int i = 0; i < static_cast<int>(names.size()); ++i)
            if (names[i] == "dur") di = i;
        REQUIRE(di >= 0);

        // (dur + dur) > 50 is not a bare col-cmp-scalar, so it cannot be
        // translated to a Query; the engine applies it instead.
        dataframe::Expr pred =
            (dataframe::col(di) + dataframe::col(di)) > std::int64_t{50};
        dataframe::ScanRequest req;
        req.filters.push_back(pred);
        dataframe::ScanResult sr = dftu_utils_test::ok(src->scan(req));
        REQUIRE(sr.filters.size() == 1);
        CHECK(sr.filters[0] == dataframe::Pushed::No);

        dataframe::DataFrame got =
            run(dataframe::LazyFrame::scan(src).filter(pred).collect());
        dataframe::DataFrame all =
            run(views::detail::scan::collect(v).collect());
        std::int64_t expect = 0;
        for (std::int64_t i = 0; i < all.num_rows(); ++i)
            if (2 * bnum(all, i, "dur") > 50) ++expect;
        CHECK(expect > 0);
        CHECK(got.num_rows() == expect);
        for (std::int64_t i = 0; i < got.num_rows(); ++i)
            CHECK(2 * bnum(got, i, "dur") > 50);
    }

    TEST_CASE("ViewSource pushdown: string predicates and is_in") {
        const auto& s = shared_trace();  // 30 "read" + 20 "fwrite"
        views::detail::scan::ScanPlan v = views::detail::scan::from_file(
            dstd::String(s.gz), dstd::String(s.idx));
        auto src = dstd::make_shared<ViewSource>(v);

        const dstd::Vector<dstd::String> names = src->names();
        int ni = -1;
        for (int i = 0; i < static_cast<int>(names.size()); ++i)
            if (names[i] == "name") ni = i;
        REQUIRE(ni >= 0);

        auto pushed_as = [&](const dataframe::Expr& pred) {
            dataframe::ScanRequest req;
            req.filters.push_back(pred);
            dataframe::ScanResult sr = dftu_utils_test::ok(src->scan(req));
            REQUIRE(sr.filters.size() == 1);
            return sr.filters[0];
        };
        auto scanned = [&](const dataframe::Expr& pred) {
            dataframe::DataFrame out =
                run(dataframe::LazyFrame::scan(src).filter(pred).collect());
            dstd::Map<dstd::String, std::int64_t> by_name;
            for (std::int64_t i = 0; i < out.num_rows(); ++i)
                ++by_name[bstr(out, i, "name")];
            return by_name;
        };
        using M = dstd::Map<dstd::String, std::int64_t>;

        SUBCASE("like pushes exactly") {
            dataframe::Expr pred = dataframe::expr_str_pred(
                dataframe::StrPredOp::Like, dataframe::col(ni), "fw%");
            CHECK(pushed_as(pred) == dataframe::Pushed::Exact);
            CHECK(scanned(pred) == M{{"fwrite", 20}});
        }
        SUBCASE("starts_with and ends_with push as an escaped like") {
            dataframe::Expr pre = dataframe::expr_str_pred(
                dataframe::StrPredOp::StartsWith, dataframe::col(ni), "re");
            CHECK(pushed_as(pre) == dataframe::Pushed::Exact);
            CHECK(scanned(pre) == M{{"read", 30}});
            dataframe::Expr suf = dataframe::expr_str_pred(
                dataframe::StrPredOp::EndsWith, dataframe::col(ni), "ite");
            CHECK(pushed_as(suf) == dataframe::Pushed::Exact);
            CHECK(scanned(suf) == M{{"fwrite", 20}});
            // A wildcard in the literal is escaped, so it matches nothing here
            // rather than everything.
            dataframe::Expr lit = dataframe::expr_str_pred(
                dataframe::StrPredOp::StartsWith, dataframe::col(ni), "%");
            CHECK(pushed_as(lit) == dataframe::Pushed::Exact);
            CHECK(scanned(lit).empty());
        }
        SUBCASE("contains pushes as a case-insensitive superset, Inexact") {
            dataframe::Expr pred = dataframe::expr_str_pred(
                dataframe::StrPredOp::Contains, dataframe::col(ni), "ea");
            CHECK(pushed_as(pred) == dataframe::Pushed::Inexact);
            CHECK(scanned(pred) == M{{"read", 30}});
            // The engine re-applies the case-sensitive form over the superset.
            dataframe::Expr upper = dataframe::expr_str_pred(
                dataframe::StrPredOp::Contains, dataframe::col(ni), "EA");
            CHECK(pushed_as(upper) == dataframe::Pushed::Inexact);
            CHECK(scanned(upper).empty());
        }
        SUBCASE("regex stays with the engine") {
            dataframe::Expr pred = dataframe::expr_str_pred(
                dataframe::StrPredOp::Search, dataframe::col(ni), "^f");
            CHECK(pushed_as(pred) == dataframe::Pushed::No);
            CHECK(scanned(pred) == M{{"fwrite", 20}});
        }
        SUBCASE("is_in pushes exactly") {
            dataframe::Expr pred = dataframe::expr_is_in(
                dataframe::col(ni), dataframe::Series::strings({"read", "x"}));
            CHECK(pushed_as(pred) == dataframe::Pushed::Exact);
            CHECK(scanned(pred) == M{{"read", 30}});
            CHECK(pushed_as(~pred) == dataframe::Pushed::Exact);
            CHECK(scanned(~pred) == M{{"fwrite", 20}});
        }
    }

    TEST_CASE("ViewSource pushdown: AND/OR/NOT of col filters") {
        const auto& s = shared_trace();
        views::detail::scan::ScanPlan v = views::detail::scan::from_file(
            dstd::String(s.gz), dstd::String(s.idx));
        auto src = dstd::make_shared<ViewSource>(v);

        const dstd::Vector<dstd::String> names = src->names();
        int di = -1, ti = -1;
        for (int i = 0; i < static_cast<int>(names.size()); ++i) {
            if (names[i] == "dur") di = i;
            if (names[i] == "ts") ti = i;
        }
        REQUIRE(di >= 0);
        REQUIRE(ti >= 0);

        dataframe::DataFrame all =
            run(views::detail::scan::collect(v).collect());

        auto pushed_as = [&](const dataframe::Expr& pred) {
            dataframe::ScanRequest req;
            req.filters.push_back(pred);
            dataframe::ScanResult sr = dftu_utils_test::ok(src->scan(req));
            REQUIRE(sr.filters.size() == 1);
            return sr.filters[0];
        };
        auto rows_matching = [&](auto keep) {
            std::int64_t n = 0;
            for (std::int64_t i = 0; i < all.num_rows(); ++i)
                if (keep(bnum(all, i, "dur"), bnum(all, i, "ts"))) ++n;
            return n;
        };
        auto scanned = [&](const dataframe::Expr& pred) {
            return run(dataframe::LazyFrame::scan(src).filter(pred).collect())
                .num_rows();
        };

        SUBCASE("both conjuncts translate, so the AND is Exact") {
            dataframe::Expr pred = (dataframe::col(di) > std::int64_t{25}) &
                                   (dataframe::col(di) < std::int64_t{45});
            CHECK(pushed_as(pred) == dataframe::Pushed::Exact);
            const std::int64_t want = rows_matching(
                [](double d, double) { return d > 25 && d < 45; });
            CHECK(want > 0);
            CHECK(scanned(pred) == want);
        }

        SUBCASE(
            "one untranslatable conjunct leaves the other pushed, Inexact") {
            // The pushed half selects a superset, so the engine must re-apply
            // the predicate; marking this Exact would return the superset.
            dataframe::Expr pred =
                (dataframe::col(di) > std::int64_t{25}) &
                ((dataframe::col(di) + dataframe::col(di)) > std::int64_t{70});
            CHECK(pushed_as(pred) == dataframe::Pushed::Inexact);
            const std::int64_t want = rows_matching(
                [](double d, double) { return d > 25 && 2 * d > 70; });
            CHECK(want > 0);
            CHECK(scanned(pred) == want);
        }

        SUBCASE("both disjuncts translate, so the OR is Exact") {
            dataframe::Expr pred = (dataframe::col(di) < std::int64_t{15}) |
                                   (dataframe::col(di) > std::int64_t{45});
            CHECK(pushed_as(pred) == dataframe::Pushed::Exact);
            const std::int64_t want = rows_matching(
                [](double d, double) { return d < 15 || d > 45; });
            CHECK(want > 0);
            CHECK(scanned(pred) == want);
        }

        SUBCASE("one untranslatable disjunct sinks the whole OR") {
            // Pushing only the translatable side would drop every row the
            // other side keeps, so nothing is pushed at all.
            dataframe::Expr pred =
                (dataframe::col(di) > std::int64_t{45}) |
                ((dataframe::col(di) + dataframe::col(di)) < std::int64_t{30});
            CHECK(pushed_as(pred) == dataframe::Pushed::No);
            const std::int64_t want = rows_matching(
                [](double d, double) { return d > 45 || 2 * d < 30; });
            CHECK(want > 0);
            CHECK(scanned(pred) == want);
        }

        SUBCASE("NOT of an exact predicate is Exact") {
            dataframe::Expr pred = ~(dataframe::col(di) > std::int64_t{25});
            CHECK(pushed_as(pred) == dataframe::Pushed::Exact);
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
            dataframe::Expr pred = ~(
                (dataframe::col(di) > std::int64_t{25}) &
                ((dataframe::col(di) + dataframe::col(di)) > std::int64_t{70}));
            CHECK(pushed_as(pred) == dataframe::Pushed::No);
            const std::int64_t want = rows_matching(
                [](double d, double) { return !(d > 25 && 2 * d > 70); });
            CHECK(want > 0);
            CHECK(scanned(pred) == want);
        }

        SUBCASE("two columns combine") {
            dataframe::Expr pred = (dataframe::col(di) > std::int64_t{25}) &
                                   (dataframe::col(ti) > std::int64_t{0});
            CHECK(pushed_as(pred) == dataframe::Pushed::Exact);
            const std::int64_t want = rows_matching(
                [](double d, double t) { return d > 25 && t > 0; });
            CHECK(want > 0);
            CHECK(scanned(pred) == want);
        }
    }

    TEST_CASE(
        "ViewSource pushdown: projection harvests only selected columns") {
        const auto& s = shared_trace();
        views::detail::scan::ScanPlan v = views::detail::scan::from_file(
            dstd::String(s.gz), dstd::String(s.idx));
        auto src = dstd::make_shared<ViewSource>(v);

        dataframe::DataFrame proj = run(
            dataframe::LazyFrame::scan(src).select({"cat", "name"}).collect());
        REQUIRE(proj.columns.size() == 2);
        CHECK(bhas(proj, "cat"));
        CHECK(bhas(proj, "name"));
        CHECK(proj.num_rows() == 50);
        for (std::int64_t i = 0; i < proj.num_rows(); ++i) {
            const dstd::String cat = bstr(proj, i, "cat");
            CHECK((cat == "POSIX" || cat == "STDIO"));
        }
    }

    TEST_CASE("ViewSource head(10) early-stops the scan (cancellation)") {
        const auto& s = shared_trace();  // 50 events
        views::detail::scan::ScanPlan v = views::detail::scan::from_file(
            dstd::String(s.gz), dstd::String(s.idx));
        auto src = dstd::make_shared<ViewSource>(v);

        dataframe::DataFrame h =
            run(dataframe::LazyFrame::scan(src).head(10).collect());
        CHECK(h.num_rows() == 10);
    }

    TEST_CASE(
        "ViewSource schema() types match the actually collected columns "
        "for a statically-known select") {
        const auto& s = shared_trace();
        views::detail::scan::ScanPlan v = views::detail::scan::select(
            views::detail::scan::from_file(dstd::String(s.gz),
                                           dstd::String(s.idx)),
            {"name", "cat", "pid", "ts", "dur"});
        auto src = dstd::make_shared<ViewSource>(v);

        dataframe::Schema schema = src->schema();
        REQUIRE(schema.fields.size() == 5);

        dataframe::DataFrame got =
            run(dataframe::LazyFrame::scan(src).collect());
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
        TestEnvironment env(200);
        dstd::String gz = create_mixed_arg_trace(env);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));
        views::detail::scan::ScanPlan v = views::detail::scan::select(
            views::detail::scan::from_file(dstd::String(gz), dstd::String(idx)),
            {"name", "args.x"});
        auto src = dstd::make_shared<ViewSource>(v);

        dataframe::Schema schema = src->schema();
        REQUIRE(schema.fields.size() == 2);
        CHECK(schema.fields[0].name == "name");
        CHECK(schema.fields[1].name == "args.x");
        // A column whose type the scan infers per batch says so rather than
        // silencing the whole schema.
        CHECK(schema.fields[0].type.id == dataframe::TypeId::String);
        CHECK(schema.fields[1].type.id == dataframe::TypeId::Unknown);

        // schema() may report Unknown for a data-dependent column, but the
        // collected frame always resolves to a concrete type.
        dataframe::DataFrame got =
            run(dataframe::LazyFrame::scan(src).collect());
        for (const dataframe::Series& c : got.columns)
            CHECK(c.type() != dataframe::TypeId::Unknown);
    }

    TEST_CASE(
        "ViewSource schema() types match the buffered frame for an "
        "aggregated view") {
        TestEnvironment env(200);
        dstd::String gz = create_mixed_arg_trace(env);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));
        views::detail::scan::ScanPlan v = views::detail::scan::agg(
            views::detail::scan::group_by(
                views::detail::scan::from_file(dstd::String(gz),
                                               dstd::String(idx)),
                {GroupKey::name()}),
            {{AggOp::Count, "", "n"}});
        auto src = dstd::make_shared<ViewSource>(v);

        dataframe::Schema schema = src->schema();
        dataframe::DataFrame buf = run(views::detail::scan::collect_frame(v));
        REQUIRE(schema.fields.size() == buf.names.size());
        for (std::size_t i = 0; i < buf.columns.size(); ++i) {
            CHECK(schema.fields[i].name == buf.names[i]);
            CHECK(schema.fields[i].type.id == buf.columns[i].type());
        }
    }

    TEST_CASE("scan::stream() over a histogram aggregation yields one chunk") {
        TestEnvironment env(200);
        dstd::String gz = create_mixed_arg_trace(env);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));
        views::detail::scan::ScanPlan v = views::detail::scan::agg(
            views::detail::scan::group_by(
                views::detail::scan::from_file(dstd::String(gz),
                                               dstd::String(idx)),
                {GroupKey::cat()}),
            {{AggOp::Hist, "dur", "h"}});

        std::size_t chunks =
            dftu_utils_test::ok(dftracer::utils::default_runtime()
                                    .submit([](views::detail::scan::ScanPlan vv)
                                                -> coro::CoroTask<std::size_t> {
                                        std::size_t n = 0;
                                        auto gen =
                                            views::detail::scan::stream(vv, 1);
                                        while (auto chunk = co_await gen.next())
                                            ++n;
                                        co_return n;
                                    }(v))
                                    .get());

        CHECK(chunks == 1);
    }

    TEST_CASE(
        "ViewSource schema() reports a concrete type for an args column once "
        "the index has harvested it") {
        TestEnvironment env(200);
        dstd::String gz = create_mixed_arg_trace(env);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        // Build the index (and its harvested column types) before asking for
        // the schema, so the "x" arg's type is known from the index rather
        // than data-dependent.
        {
            StringSink sink;
            dftu_utils_test::ok(views::detail::scan::export_json(
                                    views::detail::scan::from_file(
                                        dstd::String(gz), dstd::String(idx)),
                                    sink)
                                    .get());
        }

        views::detail::scan::ScanPlan v = views::detail::scan::select(
            views::detail::scan::from_file(dstd::String(gz), dstd::String(idx)),
            {"name", "args.x"});
        auto src = dstd::make_shared<ViewSource>(v);

        dataframe::Schema schema = src->schema();
        REQUIRE(schema.fields.size() == 2);
        CHECK(schema.fields[0].name == "name");
        CHECK(schema.fields[1].name == "args.x");
        CHECK(schema.fields[0].type.id == dataframe::TypeId::String);
        // The index harvested "x" as an integer column; schema() must report
        // that concretely instead of falling back to Unknown.
        CHECK(schema.fields[1].type.id == dataframe::TypeId::Int64);
    }

    TEST_CASE("ViewSource distinct pushdown is absorbed and matches the host") {
        TestEnvironment env(200);
        const dstd::String idx_dir = env.dir() + "/distinct.dftindex";
        dstd::String gz = create_distinct_trace(env, idx_dir);
        dstd::String idx = dstd::String(determine_index_path(gz, idx_dir));

        views::detail::scan::ScanPlan v = views::detail::scan::select(
            views::detail::scan::from_file(dstd::String(gz), dstd::String(idx)),
            {"run", "metric"});
        {
            const auto& plan = *v;
            auto vdef = ok(dftracer::utils::trace::views::detail::make_vdef(
                plan, /*for_aggregation=*/false));
            std::uint64_t skipped = 0;
            auto units =
                run(dftracer::utils::trace::views::detail::gather_units(
                    plan, vdef, skipped));
            REQUIRE(units.size() > 8);
        }

        // Expected: the first row of each key in file order.
        dstd::Vector<dstd::Pair<dstd::String, dstd::String>> want;
        dstd::Set<dstd::Pair<dstd::String, dstd::String>> seen;
        for (int i = 0; i < DISTINCT_EVENTS; ++i) {
            dstd::Pair<dstd::String, dstd::String> k{distinct_run(i),
                                                     distinct_metric(i)};
            if (seen.insert(k).second) want.push_back(k);
        }
        REQUIRE(want.size() > 20);

        dataframe::LazyFrame lf = views::detail::scan::collect(v);
        // The plan says the source does the distinct.
        CHECK(lf.unique().explain().find("unique") == dstd::String::npos);
        CHECK(lf.drop_duplicates().explain().find("unique") ==
              dstd::String::npos);

        for (int round = 0; round < 3; ++round) {
            dataframe::DataFrame pushed = run(lf.unique().collect());
            REQUIRE(pushed.num_rows() ==
                    static_cast<std::int64_t>(want.size()));
            for (std::int64_t i = 0; i < pushed.num_rows(); ++i) {
                CHECK(bstr(pushed, i, "args.run") ==
                      want[static_cast<std::size_t>(i)].first);
                CHECK(bstr(pushed, i, "args.metric") ==
                      want[static_cast<std::size_t>(i)].second);
            }
        }

        // A subset keys only that column and keeps the first row per key.
        dataframe::LazyFrame lf2 =
            views::detail::scan::collect(views::detail::scan::select(
                views::detail::scan::from_file(dstd::String(gz),
                                               dstd::String(idx)),
                {"run", "metric"}));
        CHECK(lf2.unique({"args.run"}).explain().find("unique") ==
              dstd::String::npos);
        dataframe::DataFrame sub = run(lf2.unique({"args.run"}).collect());
        dstd::Vector<dstd::Pair<dstd::String, dstd::String>> want_sub;
        dstd::Set<dstd::String> seen_run;
        for (int i = 0; i < DISTINCT_EVENTS; ++i)
            if (seen_run.insert(distinct_run(i)).second)
                want_sub.emplace_back(distinct_run(i), distinct_metric(i));
        REQUIRE(sub.num_rows() == static_cast<std::int64_t>(want_sub.size()));
        for (std::int64_t i = 0; i < sub.num_rows(); ++i) {
            CHECK(bstr(sub, i, "args.run") ==
                  want_sub[static_cast<std::size_t>(i)].first);
            CHECK(bstr(sub, i, "args.metric") ==
                  want_sub[static_cast<std::size_t>(i)].second);
        }
    }

    TEST_CASE("ViewSource refuses a distinct over a computed column") {
        TestEnvironment env(200);
        const dstd::String idx_dir = env.dir() + "/distinct.dftindex";
        dstd::String gz = create_distinct_trace(env, idx_dir);
        dstd::String idx = dstd::String(determine_index_path(gz, idx_dir));
        views::detail::scan::ScanPlan v = views::detail::scan::select(
            views::detail::scan::from_file(dstd::String(gz), dstd::String(idx)),
            {"io_cat"});
        dataframe::LazyFrame lf = views::detail::scan::collect(v);
        CHECK(lf.unique().explain().find("unique") != dstd::String::npos);
        // io_cat comes from the event name, which a scan key cannot read as
        // a field, so the host cursor must see the real categories.
        const std::int64_t host =
            dftu_utils_test::ok(
                run(views::detail::scan::collect(
                        views::detail::scan::select(
                            views::detail::scan::from_file(dstd::String(gz),
                                                           dstd::String(idx)),
                            {"io_cat"}))
                        .collect())
                    .unique())
                .num_rows();
        CHECK(host > 1);
        CHECK(run(lf.unique().collect()).num_rows() == host);
    }
}
