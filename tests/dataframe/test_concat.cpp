// Vertical concatenation and union, eager and lazy, through the C++ facade,
// the C ABI and the op registry.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using dftracer::utils::coro::CoroTask;
using dftracer::utils::dataframe::col;
using dftracer::utils::dataframe::ConcatHow;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::InMemorySource;
using dftracer::utils::dataframe::LazyFrame;
using dftracer::utils::dataframe::OpArgs;
using dftracer::utils::dataframe::Schema;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;

namespace {

DataFrame run(CoroTask<DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

Series i64s(const std::vector<std::int64_t>& v,
            const std::vector<std::uint8_t>& validity = {}) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()),
                            validity.empty() ? nullptr : validity.data());
}

// a: k = [1, 2, null], s = [x, y, z]
DataFrame make_a() {
    DataFrame df;
    df.names = {"k", "s"};
    df.columns.push_back(i64s({1, 2, 0}, {0x03}));
    df.columns.push_back(Series::strings({"x", "y", "z"}));
    return df;
}

// b: k = [2, 4], s = [y, w]
DataFrame make_b() {
    DataFrame df;
    df.names = {"k", "s"};
    df.columns.push_back(i64s({2, 4}));
    df.columns.push_back(Series::strings({"y", "w"}));
    return df;
}

using I = std::optional<std::int64_t>;
using S = std::optional<std::string>;
const I NI = std::nullopt;

std::vector<I> i64_col(const DataFrame& df, const std::string& name) {
    Series c = df.column(name);
    REQUIRE(c.valid());
    c = c.materialize();
    std::vector<I> out;
    for (std::int64_t i = 0; i < c.length(); ++i) {
        if (c.is_null(i))
            out.emplace_back(std::nullopt);
        else
            out.emplace_back(c.data<std::int64_t>()[i]);
    }
    return out;
}

std::vector<S> str_col(const DataFrame& df, const std::string& name) {
    Series c = df.column(name);
    REQUIRE(c.valid());
    c = c.materialize();
    std::vector<S> out;
    for (std::int64_t i = 0; i < c.length(); ++i) {
        if (c.is_null(i))
            out.emplace_back(std::nullopt);
        else
            out.emplace_back(std::string(c.string_at(i)));
    }
    return out;
}

dftu_dataframe* to_abi(const DataFrame& df) {
    std::vector<const char*> names;
    std::vector<dftu_series*> cols;
    for (std::size_t i = 0; i < df.names.size(); ++i) {
        names.push_back(df.names[i].c_str());
        cols.push_back(df.columns[i].share().release());
    }
    return dftu_dataframe_new(names.data(), cols.data(),
                              static_cast<int32_t>(names.size()));
}

}  // namespace

TEST_SUITE("dataframe concat") {
    TEST_CASE("eager vertical keeps every row in order, nulls included") {
        DataFrame a = make_a();
        DataFrame b = make_b();
        DataFrame out = dftracer::utils::dataframe::concat({&a, &b});
        CHECK(out.names == std::vector<std::string>{"k", "s"});
        CHECK(i64_col(out, "k") == std::vector<I>{1, 2, NI, 2, 4});
        CHECK(str_col(out, "s") == std::vector<S>{"x", "y", "z", "y", "w"});
    }

    TEST_CASE("lazy parity with eager under one and many morsels") {
        DataFrame a = make_a();
        DataFrame b = make_b();
        DataFrame eager = dftracer::utils::dataframe::concat({&a, &b});
        for (std::int64_t morsel : std::vector<std::int64_t>{0, 1, 2}) {
            LazyFrame la =
                LazyFrame::scan(std::make_shared<InMemorySource>(make_a()));
            LazyFrame lb =
                LazyFrame::scan(std::make_shared<InMemorySource>(make_b()));
            DataFrame out = run(la.concat(lb).collect(morsel));
            CHECK(out.names == eager.names);
            CHECK(i64_col(out, "k") == i64_col(eager, "k"));
            CHECK(str_col(out, "s") == str_col(eager, "s"));
        }
    }

    TEST_CASE("lazy schema, output_schema and explain") {
        LazyFrame la =
            LazyFrame::scan(std::make_shared<InMemorySource>(make_a()));
        LazyFrame lb =
            LazyFrame::scan(std::make_shared<InMemorySource>(make_b()));
        LazyFrame plan = la.concat(lb);
        CHECK(plan.schema() == std::vector<std::string>{"k", "s"});
        Schema s = plan.output_schema();
        REQUIRE(s.fields.size() == 2);
        CHECK(s.fields[0].type.id == TypeId::Int64);
        CHECK(s.fields[1].type.id == TypeId::String);
        CHECK(plan.explain().find("concat") != std::string::npos);
    }

    TEST_CASE("a filter after the concat applies to both sides") {
        LazyFrame la =
            LazyFrame::scan(std::make_shared<InMemorySource>(make_a()));
        LazyFrame lb =
            LazyFrame::scan(std::make_shared<InMemorySource>(make_b()));
        LazyFrame plan = la.concat(lb).filter(col(0) > std::int64_t{1});
        std::string text = plan.explain();
        CHECK(text.find("concat") < text.find("filter"));
        DataFrame out = run(plan.collect(1));
        CHECK(i64_col(out, "k") == std::vector<I>{2, 2, 4});
        CHECK(str_col(out, "s") == std::vector<S>{"y", "y", "w"});
    }

    TEST_CASE("the right side may itself be lazy work") {
        LazyFrame la =
            LazyFrame::scan(std::make_shared<InMemorySource>(make_a()));
        LazyFrame lb =
            LazyFrame::scan(std::make_shared<InMemorySource>(make_b()))
                .filter(col(0) > std::int64_t{3});
        DataFrame out = run(la.concat(lb).collect());
        CHECK(i64_col(out, "k") == std::vector<I>{1, 2, NI, 4});
    }

    TEST_CASE("concat then unique is union") {
        LazyFrame la =
            LazyFrame::scan(std::make_shared<InMemorySource>(make_a()));
        LazyFrame lb =
            LazyFrame::scan(std::make_shared<InMemorySource>(make_b()));
        DataFrame out = run(la.concat(lb).unique().collect());
        CHECK(i64_col(out, "k") == std::vector<I>{1, 2, NI, 4});
        CHECK(str_col(out, "s") == std::vector<S>{"x", "y", "z", "w"});
    }

    TEST_CASE("refusals name the problem") {
        LazyFrame la =
            LazyFrame::scan(std::make_shared<InMemorySource>(make_a()));
        DataFrame fewer;
        fewer.names = {"k"};
        fewer.columns.push_back(i64s({1}));
        DataFrame renamed = make_b();
        renamed.names = {"k", "t"};
        DataFrame retyped;
        retyped.names = {"k", "s"};
        retyped.columns.push_back(Series::strings({"1"}));
        retyped.columns.push_back(Series::strings({"q"}));
        CHECK_THROWS_WITH_AS(
            la.concat(LazyFrame::scan(
                std::make_shared<InMemorySource>(std::move(fewer)))),
            "concat: column count differs (2 vs 1)", std::invalid_argument);
        CHECK_THROWS_WITH_AS(
            la.concat(LazyFrame::scan(
                std::make_shared<InMemorySource>(std::move(renamed)))),
            "concat: column 1 is 's' vs 't'", std::invalid_argument);
        CHECK_THROWS_WITH_AS(
            la.concat(LazyFrame::scan(
                std::make_shared<InMemorySource>(std::move(retyped)))),
            "concat: column 'k' type differs", std::invalid_argument);
    }

    TEST_CASE("C ABI: concat, concat2, union and lazy concat") {
        dftu_dataframe* ah = to_abi(make_a());
        dftu_dataframe* bh = to_abi(make_b());
        REQUIRE(ah);
        REQUIRE(bh);
        const dftu_dataframe* parts[2] = {ah, bh};
        dftu_dataframe* c =
            dftu_dataframe_concat(parts, 2, DFTU_CONCAT_VERTICAL);
        REQUIRE(c);
        CHECK(dftu_dataframe_num_rows(c) == 5);
        CHECK(dftu_dataframe_num_columns(c) == 2);
        dftu_dataframe* c2 =
            dftu_dataframe_concat2(ah, bh, DFTU_CONCAT_VERTICAL);
        REQUIRE(c2);
        CHECK(dftu_dataframe_num_rows(c2) == 5);
        dftu_dataframe* u = dftu_dataframe_union(ah, bh);
        REQUIRE(u);
        CHECK(dftu_dataframe_num_rows(u) == 4);
        CHECK(dftu_dataframe_concat(parts, 0, DFTU_CONCAT_VERTICAL) == nullptr);
        CHECK(dftu_dataframe_concat(nullptr, 2, DFTU_CONCAT_VERTICAL) ==
              nullptr);
        dftu_concat_how bad_how;  // no in-range non-enumerator exists
        const int bad_how_bits = 7;
        std::memcpy(&bad_how, &bad_how_bits, sizeof(bad_how));
        CHECK(dftu_dataframe_concat2(ah, bh, bad_how) == nullptr);

        dftu_lazyframe* la = dftu_dataframe_lazy(ah);
        dftu_lazyframe* lb = dftu_dataframe_lazy(bh);
        REQUIRE(la);
        REQUIRE(lb);
        dftu_lazyframe* lc = dftu_lazyframe_concat(la, lb);
        REQUIRE(lc);
        char* text = dftu_lazyframe_explain(lc);
        REQUIRE(text);
        CHECK(std::string(text).find("concat") != std::string::npos);
        dftu_query_string_free(text);
        dftu_dataframe* collected = dftu_lazyframe_collect(lc, 0);
        REQUIRE(collected);
        CHECK(dftu_dataframe_num_rows(collected) == 5);
        CHECK(dftu_lazyframe_concat(la, nullptr) == nullptr);

        const char* only_k[1] = {"k"};
        dftu_dataframe* one = dftu_dataframe_select(ah, only_k, 1);
        dftu_lazyframe* lone = dftu_dataframe_lazy(one);
        REQUIRE(lone);
        CHECK(dftu_lazyframe_concat(la, lone) == nullptr);

        dftu_lazyframe_free(lone);
        dftu_dataframe_free(one);
        dftu_dataframe_free(collected);
        dftu_lazyframe_free(lc);
        dftu_lazyframe_free(lb);
        dftu_lazyframe_free(la);
        dftu_dataframe_free(u);
        dftu_dataframe_free(c2);
        dftu_dataframe_free(c);
        dftu_dataframe_free(bh);
        dftu_dataframe_free(ah);
    }

    TEST_CASE(
        "registry: dftu.frame.concat, dftu.frame.union, dftu.lazy.concat") {
        dftu_dataframe* ah = to_abi(make_a());
        dftu_dataframe* bh = to_abi(make_b());
        REQUIRE(ah);
        REQUIRE(bh);
        const dftu_dataframe* fin[2] = {ah, bh};

        const dftu_op_desc* concat_op = dftu_op_find("dftu.frame.concat");
        REQUIRE(concat_op);
        CHECK(dftu_op_arity(concat_op->sig) == 2);
        OpArgs carg;
        carg.i32(2, DFTU_CONCAT_VERTICAL);
        dftu_dataframe* c = dftu_op_run_frame(concat_op, fin, 2, carg);
        REQUIRE(c);
        CHECK(dftu_dataframe_num_rows(c) == 5);
        CHECK(dftu_op_run_frame(concat_op, fin, 1, carg) == nullptr);

        const dftu_op_desc* union_op = dftu_op_find("dftu.frame.union");
        REQUIRE(union_op);
        dftu_dataframe* u = dftu_op_run_frame(union_op, fin, 2, nullptr);
        REQUIRE(u);
        CHECK(dftu_dataframe_num_rows(u) == 4);

        dftu_lazyframe* la = dftu_dataframe_lazy(ah);
        dftu_lazyframe* lb = dftu_dataframe_lazy(bh);
        const dftu_lazyframe* lin[2] = {la, lb};
        const dftu_op_desc* lazy_op = dftu_op_find("dftu.lazy.concat");
        REQUIRE(lazy_op);
        CHECK(dftu_op_arity(lazy_op->sig) == 2);
        dftu_lazyframe* lc = dftu_op_run_lazy(lazy_op, lin, 2, nullptr);
        REQUIRE(lc);
        dftu_dataframe* collected = dftu_lazyframe_collect(lc, 0);
        REQUIRE(collected);
        CHECK(dftu_dataframe_num_rows(collected) == 5);

        dftu_dataframe_free(collected);
        dftu_lazyframe_free(lc);
        dftu_lazyframe_free(lb);
        dftu_lazyframe_free(la);
        dftu_dataframe_free(u);
        dftu_dataframe_free(c);
        dftu_dataframe_free(bh);
        dftu_dataframe_free(ah);
    }
}
