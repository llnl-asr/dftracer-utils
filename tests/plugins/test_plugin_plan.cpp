// SPDX-License-Identifier: MIT
// The plan hooks of a plugin source and file format (tests/plugins/
// plan_plugin.c and sdk_plan_plugin.cpp): the votes of a plan decide which
// filters the engine keeps, a token the plan gives reaches scan(), explain
// prints the plan, a plugin that reads duql gets the filters as one duql text,
// a failing plan fails the query, and a plugin without a plan is unchanged.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/provider_registry.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

#ifndef PLAN_PLUGIN_PATH
#error "PLAN_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef SDK_PLAN_PLUGIN_PATH
#error "SDK_PLAN_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

dstd::Optional<plugins::host::Plugins> load(
    std::initializer_list<const char*> paths) {
    plugins::host::Plugins::Builder builder;
    for (const char* path : paths) builder.add(dstd::String(path));
    auto built = builder.build();
    const dstd::String failure = built.has_value()
                                     ? dstd::String()
                                     : dstd::String(built.error().message());
    CAPTURE(failure);
    REQUIRE(built.has_value());
    return std::move(built).value();
}

LazyFrame provider(dstd::StringView name) {
    auto factory = find_provider(name);
    REQUIRE(factory != nullptr);
    return LazyFrame::scan(factory->make_source());
}

Expr cmp_i64(CmpOp op, std::int32_t column, std::int64_t value) {
    dftu_scalar s;
    s.kind = DFTU_SCALAR_TAG_I64;
    s.value.i = value;
    return expr_cmp(op, col(column), s);
}

Expr id_above(std::int64_t value) { return cmp_i64(CmpOp::Gt, 0, value); }

dstd::String analyze(const LazyFrame& lf) {
    return dftu_utils_test::ok(
        default_runtime().submit(lf.explain_analyze()).get());
}

DataFrame run(const LazyFrame& lf) {
    return dftu_utils_test::ok(default_runtime().submit(lf.collect()).get());
}

dstd::Vector<std::int64_t> column_of(const DataFrame& df,
                                     dstd::StringView name) {
    if (df.num_rows() == 0) return {};
    Series column = df.column(name);
    REQUIRE(column.valid());
    if (!column.is_flat()) column = column.materialize();
    const std::int64_t* data = column.data<std::int64_t>();
    return data ? dstd::Vector<std::int64_t>(data, data + column.length())
                : dstd::Vector<std::int64_t>{};
}

bool has(dstd::StringView text, dstd::StringView piece) {
    return text.find(piece) != dstd::StringView::npos;
}

}  // namespace

TEST_CASE("the votes of a plan decide which filters the engine keeps") {
    auto plugins = load({PLAN_PLUGIN_PATH});
    const LazyFrame lf = provider("plan_plugin.src").filter(id_above(2));
    // The plan voted EXACT while scan() wrote NO, so the engine dropped the
    // filter and the rows are the ones scan() applied by the token.
    CHECK(column_of(run(lf), "id") == dstd::Vector<std::int64_t>{3, 4, 5, 6});
    const dstd::String text = analyze(lf);
    CAPTURE(text);
    CHECK(has(text, "pushed filter 0: exact"));
    CHECK_FALSE(has(text, "\n  filter: rows in"));
}

TEST_CASE("an out-of-range vote of a plan reads as no") {
    auto plugins = load({PLAN_PLUGIN_PATH});
    const LazyFrame lf =
        provider("plan_plugin.src").filter(cmp_i64(CmpOp::Gt, 1, 20));
    CHECK(column_of(run(lf), "id") == dstd::Vector<std::int64_t>{3, 4, 5, 6});
    CHECK(has(analyze(lf), "pushed filter 0: no"));
}

TEST_CASE("the token of a plan reaches scan, and the plan runs once per scan") {
    auto plugins = load({PLAN_PLUGIN_PATH});
    const dstd::String text =
        analyze(provider("plan_plugin.src").filter(id_above(4)));
    CAPTURE(text);
    CHECK(has(text, "counter plan_plugin.src.token: 4"));
    CHECK(has(text, "counter plan_plugin.src.plans: 1"));
    CHECK(has(text, "counter plan_plugin.src.scans: 1"));
}

TEST_CASE("explain prints the votes, the cost and the rows of a plan") {
    auto plugins = load({PLAN_PLUGIN_PATH});
    const dstd::String plan =
        provider("plan_plugin.src").filter(id_above(2)).explain();
    CAPTURE(plan);
    CHECK(has(plan,
              "provider 'plan_plugin.src': plan_votes=exact; plan_cost=10; "
              "plan_est_rows=4"));
    const dstd::String unfiltered = provider("plan_plugin.src").explain();
    CHECK(has(unfiltered, "plan_votes=none; plan_cost=15; plan_est_rows=6"));
}

TEST_CASE("a source without a plan hook is unchanged") {
    auto plugins = load({PLAN_PLUGIN_PATH});
    const LazyFrame lf = provider("plan_plugin.noplan").filter(id_above(2));
    CHECK(lf.explain() == "scan [id, v]\nfilter");
    CHECK(column_of(run(lf), "id") == dstd::Vector<std::int64_t>{3, 4, 5, 6});
    // The vote is the one scan() wrote.
    CHECK(has(analyze(lf), "pushed filter 0: inexact"));
}

TEST_CASE("a plan error fails the query with the message of the plugin") {
    auto plugins = load({PLAN_PLUGIN_PATH});
    const LazyFrame lf = provider("plan_plugin.fail").filter(id_above(2));
    auto collected = default_runtime().submit(lf.collect()).get();
    REQUIRE_FALSE(collected.has_value());
    CHECK(has(collected.error().message(), "plan_plugin: the plan fails"));
    auto analyzed = default_runtime().submit(lf.explain_analyze()).get();
    REQUIRE_FALSE(analyzed.has_value());
    CHECK(has(analyzed.error().message(), "plan_plugin: the plan fails"));
    // An explain has no error channel, so it prints the message.
    CHECK(has(lf.explain(), "plan_error="));
    CHECK(has(lf.explain(), "plan_plugin: the plan fails"));
}

TEST_CASE("plan_duql gets the filters it can read as one duql text") {
    auto plugins = load({PLAN_PLUGIN_PATH});
    const Expr search = expr_str_pred(StrPredOp::Search, col(1), "x");
    const dstd::String plan = provider("plan_plugin.dq")
                                  .filter(id_above(2))
                                  .filter(search)
                                  .filter(cmp_i64(CmpOp::Le, 1, 50))
                                  .explain();
    CAPTURE(plan);
    // The regex filter is not duql; it keeps its place in the request, so the
    // indexes of the others are 0 and 2.
    CHECK(has(plan,
              "provider 'plan_plugin.dq': duql=id > 2 and v <= 50; "
              "indexes=0,2; seen_est_rows=222; plan_votes=inexact,no,inexact; "
              "plan_cost=unknown; "
              "plan_est_rows=222"));
}

TEST_CASE("plan runs when no filter is expressible in duql") {
    auto plugins = load({PLAN_PLUGIN_PATH});
    const Expr search = expr_str_pred(StrPredOp::Search, col(1), "x");
    const dstd::String plan =
        provider("plan_plugin.dq").filter(search).explain();
    CAPTURE(plan);
    CHECK(has(plan,
              "duql=plan; indexes=; seen_est_rows=111; plan_votes=no; "
              "plan_cost=unknown; "
              "plan_est_rows=111"));
}

TEST_CASE(
    "a plugin with plan_duql only has no plan for inexpressible filters") {
    auto plugins = load({PLAN_PLUGIN_PATH});
    const Expr search = expr_str_pred(StrPredOp::Search, col(1), "x");
    CHECK_FALSE(has(provider("plan_plugin.dq_only").filter(search).explain(),
                    "plan_votes"));
    const dstd::String plan =
        provider("plan_plugin.dq_only").filter(id_above(1)).explain();
    CAPTURE(plan);
    CHECK(has(plan,
              "duql=id > 1; indexes=0; seen_est_rows=222; plan_votes=inexact; "
              "plan_cost=unknown; "
              "plan_est_rows=222"));
}

TEST_CASE("an SDK source plans and scan reads the token") {
    auto plugins = load({SDK_PLAN_PLUGIN_PATH});
    const LazyFrame lf = provider("sdk_plan.src").filter(id_above(1));
    CHECK(column_of(run(lf), "tok") == dstd::Vector<std::int64_t>{42, 42});
    CHECK(has(lf.explain(),
              "provider 'sdk_plan.src': plan_votes=inexact; plan_cost=7.5; "
              "plan_est_rows=3"));
    const LazyFrame plain = provider("sdk_plan.plain");
    CHECK(column_of(run(plain), "tok") ==
          dstd::Vector<std::int64_t>{-1, -1, -1});
}

TEST_CASE("an SDK source gets the duql text of its filters") {
    auto plugins = load({SDK_PLAN_PLUGIN_PATH});
    const LazyFrame lf =
        provider("sdk_plan.dq").filter(id_above(1)).filter(id_above(2));
    const dstd::String plan = lf.explain();
    CAPTURE(plan);
    CHECK(has(plan, "duql=id > 1 and id > 2; plan_votes=inexact,inexact"));
    // The token is the length of the duql text, and the engine re-applies the
    // inexact filters.
    CHECK(column_of(run(lf), "tok") == dstd::Vector<std::int64_t>{17});
}

namespace {

struct Scratch {
    dstd::Path root = dftu_utils_test::make_unique_test_path("plan");
    dstd::String data;
    dstd::String index;

    Scratch() {
        data = dstd::String((root / "data").str());
        index = dstd::String((root / "index").str());
        REQUIRE(dstd::fs::create_directories(dstd::PathView(data)).has_value());
        REQUIRE(
            dstd::fs::create_directories(dstd::PathView(index)).has_value());
    }
};

dstd::String write_rows(dstd::StringView dir, std::int64_t rows) {
    dstd::String text = "SDKP\n";
    for (std::int64_t i = 1; i <= rows; ++i) text += dstd::to_string(i) + "\n";
    const dstd::String path = dstd::String(dir) + "/a.sdkp";
    REQUIRE(dstd::fs::write_file(dstd::PathView(path), text).has_value());
    return path;
}

}  // namespace

TEST_CASE("a file format plans once per scan and gives every chunk the token") {
    Scratch s;
    auto plugins = load({SDK_PLAN_PLUGIN_PATH});
    const dstd::String path = write_rows(s.data, 5);
    const LazyFrame lf =
        LazyFrame::scan_file(path, s.index).filter(id_above(2));
    const dstd::String plan = lf.explain();
    CAPTURE(plan);
    CHECK(has(plan,
              "file format 'sdk_plan.fmt': chunks=1 of 1 read; duql=id > 2; "
              "plan_votes=no; plan_cost=unknown; plan_est_rows=1001"));
    const dstd::String text = analyze(lf);
    CAPTURE(text);
    CHECK(has(text, "counter sdk_plan.fmt.token: 7"));
    CHECK(column_of(run(lf), "id") == dstd::Vector<std::int64_t>{3, 4, 5});
}
