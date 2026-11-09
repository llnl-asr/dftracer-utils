// SPDX-License-Identifier: MIT
// Hash join, eager and lazy: every JoinHow against hand-computed rows, with
// the lazy path checked for parity against the eager one under morsels small
// enough that the right-preserving flush runs after several probes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

DataFrame run(coro::CoroTask<dstd::Result<DataFrame>> t) {
    return dftu_utils_test::ok(
        dftracer::utils::default_runtime().submit(std::move(t)).get());
}

Series i64s(const dstd::Vector<std::int64_t>& v,
            const dstd::Vector<std::uint8_t>& validity = {}) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()),
                            validity.empty() ? nullptr : validity.data());
}

Series bools(const dstd::Vector<bool>& v) {
    dstd::Vector<std::uint8_t> bits((v.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i]) bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    return Series::flat(TypeId::Bool, bits.data(),
                        static_cast<std::int64_t>(v.size()));
}

// left: k = [1, 2, 3, 2, null], v = [10, 20, 30, 40, 50], flag = [t,f,t,f,t]
DataFrame make_left() {
    DataFrame df;
    df.names = {"k", "v", "flag"};
    df.columns.push_back(i64s({1, 2, 3, 2, 0}, {0x0f}));
    df.columns.push_back(i64s({10, 20, 30, 40, 50}));
    df.columns.push_back(bools({true, false, true, false, true}));
    return df;
}

// right: k = [2, 4, 2, null], name = [b, d, bb, n], v = [200, 400, 220, 0]
DataFrame make_right() {
    DataFrame df;
    df.names = {"k", "name", "v"};
    df.columns.push_back(i64s({2, 4, 2, 0}, {0x07}));
    df.columns.push_back(Series::strings({"b", "d", "bb", "n"}));
    df.columns.push_back(i64s({200, 400, 220, 0}));
    return df;
}

dstd::Vector<dstd::Optional<std::int64_t>> i64_col(const DataFrame& df,
                                                   const dstd::String& name) {
    Series c = df.column(name);
    REQUIRE(c.valid());
    c = c.materialize();
    dstd::Vector<dstd::Optional<std::int64_t>> out;
    for (std::int64_t i = 0; i < c.length(); ++i) {
        if (c.is_null(i))
            out.emplace_back(dstd::nullopt);
        else
            out.emplace_back(c.data<std::int64_t>()[i]);
    }
    return out;
}

dstd::Vector<dstd::Optional<dstd::String>> str_col(const DataFrame& df,
                                                   const dstd::String& name) {
    Series c = df.column(name);
    REQUIRE(c.valid());
    c = c.materialize();
    dstd::Vector<dstd::Optional<dstd::String>> out;
    for (std::int64_t i = 0; i < c.length(); ++i) {
        if (c.is_null(i))
            out.emplace_back(dstd::nullopt);
        else
            out.emplace_back(dstd::String(c.string_at(i)));
    }
    return out;
}

dstd::Vector<dstd::Optional<bool>> bool_col(const DataFrame& df,
                                            const dstd::String& name) {
    Series c = df.column(name);
    REQUIRE(c.valid());
    c = c.materialize();
    REQUIRE(c.type() == TypeId::Bool);
    dstd::Vector<dstd::Optional<bool>> out;
    const auto* bits = c.data<std::uint8_t>();
    for (std::int64_t i = 0; i < c.length(); ++i) {
        if (c.is_null(i))
            out.emplace_back(dstd::nullopt);
        else
            out.emplace_back(((bits[i >> 3] >> (i & 7)) & 1) != 0);
    }
    return out;
}

using I = dstd::Optional<std::int64_t>;
using S = dstd::Optional<dstd::String>;
using B = dstd::Optional<bool>;
const I NI = dstd::nullopt;
const S NS = dstd::nullopt;
const B NB = dstd::nullopt;

DataFrame lazy_join(JoinHow how, std::int64_t morsel_rows) {
    LazyFrame left =
        LazyFrame::scan(dstd::make_shared<InMemorySource>(make_left()));
    LazyFrame right =
        LazyFrame::scan(dstd::make_shared<InMemorySource>(make_right()));
    return run(left.join(right, {"k"}, how).collect(morsel_rows));
}

void check_same(const DataFrame& a, const DataFrame& b) {
    REQUIRE(a.names == b.names);
    REQUIRE(a.num_rows() == b.num_rows());
    for (const dstd::String& n : a.names) {
        Series ca = a.column(n);
        Series cb = b.column(n);
        REQUIRE(ca.type() == cb.type());
        if (ca.type() == TypeId::Int64)
            CHECK(i64_col(a, n) == i64_col(b, n));
        else if (ca.type() == TypeId::String)
            CHECK(str_col(a, n) == str_col(b, n));
        else if (ca.type() == TypeId::Bool)
            CHECK(bool_col(a, n) == bool_col(b, n));
        else
            FAIL("unexpected column type in join parity check");
    }
}

}  // namespace

TEST_SUITE("dataframe join") {
    TEST_CASE("inner: matched pairs in left order, chains in right order") {
        DataFrame out =
            dftu_utils_test::ok(make_left().join(make_right(), {"k"}));
        CHECK(out.names ==
              dstd::Vector<dstd::String>{"k", "v", "flag", "name", "v_right"});
        CHECK(i64_col(out, "k") == dstd::Vector<I>{2, 2, 2, 2});
        CHECK(i64_col(out, "v") == dstd::Vector<I>{20, 20, 40, 40});
        CHECK(str_col(out, "name") == dstd::Vector<S>{"b", "bb", "b", "bb"});
        CHECK(i64_col(out, "v_right") == dstd::Vector<I>{200, 220, 200, 220});
        CHECK(bool_col(out, "flag") ==
              dstd::Vector<B>{false, false, false, false});
    }

    TEST_CASE("left: unmatched and null-keyed left rows keep null right") {
        DataFrame out = dftu_utils_test::ok(
            make_left().join(make_right(), {"k"}, JoinHow::Left));
        CHECK(i64_col(out, "k") == dstd::Vector<I>{1, 2, 2, 3, 2, 2, NI});
        CHECK(i64_col(out, "v") == dstd::Vector<I>{10, 20, 20, 30, 40, 40, 50});
        CHECK(str_col(out, "name") ==
              dstd::Vector<S>{NS, "b", "bb", NS, "b", "bb", NS});
        CHECK(i64_col(out, "v_right") ==
              dstd::Vector<I>{NI, 200, 220, NI, 200, 220, NI});
        CHECK(bool_col(out, "flag") ==
              dstd::Vector<B>{true, false, false, true, false, false, true});
    }

    TEST_CASE("right: matches then unmatched right rows, key from the right") {
        DataFrame out = dftu_utils_test::ok(
            make_left().join(make_right(), {"k"}, JoinHow::Right));
        CHECK(i64_col(out, "k") == dstd::Vector<I>{2, 2, 2, 2, 4, NI});
        CHECK(i64_col(out, "v") == dstd::Vector<I>{20, 20, 40, 40, NI, NI});
        CHECK(bool_col(out, "flag") ==
              dstd::Vector<B>{false, false, false, false, NB, NB});
        CHECK(str_col(out, "name") ==
              dstd::Vector<S>{"b", "bb", "b", "bb", "d", "n"});
        CHECK(i64_col(out, "v_right") ==
              dstd::Vector<I>{200, 220, 200, 220, 400, 0});
    }

    TEST_CASE("outer: both sides' unmatched rows") {
        DataFrame out = dftu_utils_test::ok(
            make_left().join(make_right(), {"k"}, JoinHow::Outer));
        CHECK(i64_col(out, "k") ==
              dstd::Vector<I>{1, 2, 2, 3, 2, 2, NI, 4, NI});
        CHECK(i64_col(out, "v") ==
              dstd::Vector<I>{10, 20, 20, 30, 40, 40, 50, NI, NI});
        CHECK(str_col(out, "name") ==
              dstd::Vector<S>{NS, "b", "bb", NS, "b", "bb", NS, "d", "n"});
        CHECK(i64_col(out, "v_right") ==
              dstd::Vector<I>{NI, 200, 220, NI, 200, 220, NI, 400, 0});
    }

    TEST_CASE("semi and anti: left columns only, one row per left row") {
        DataFrame semi = dftu_utils_test::ok(
            make_left().join(make_right(), {"k"}, JoinHow::Semi));
        CHECK(semi.names == dstd::Vector<dstd::String>{"k", "v", "flag"});
        CHECK(i64_col(semi, "v") == dstd::Vector<I>{20, 40});
        DataFrame anti = dftu_utils_test::ok(
            make_left().join(make_right(), {"k"}, JoinHow::Anti));
        CHECK(anti.names == dstd::Vector<dstd::String>{"k", "v", "flag"});
        CHECK(i64_col(anti, "v") == dstd::Vector<I>{10, 30, 50});
        CHECK(i64_col(anti, "k") == dstd::Vector<I>{1, 3, NI});
    }

    TEST_CASE("cross: every pair, keys ignored") {
        DataFrame out = dftu_utils_test::ok(
            make_left().join(make_right(), {}, JoinHow::Cross));
        CHECK(out.num_rows() == 20);
        CHECK(out.names == dstd::Vector<dstd::String>{
                               "k", "v", "flag", "k_right", "name", "v_right"});
        dstd::Vector<I> k = i64_col(out, "k");
        dstd::Vector<I> kr = i64_col(out, "k_right");
        CHECK(k[0] == I{1});
        CHECK(k[3] == I{1});
        CHECK(k[4] == I{2});
        CHECK(kr[0] == I{2});
        CHECK(kr[1] == I{4});
        CHECK(kr[3] == NI);
        CHECK(kr[4] == I{2});
    }

    TEST_CASE("different key names keep both key columns") {
        DataFrame right =
            dftu_utils_test::ok(make_right().rename({"rk", "name", "rv"}));
        DataFrame out =
            dftu_utils_test::ok(make_left().join(right, {"k"}, {"rk"}));
        CHECK(out.names ==
              dstd::Vector<dstd::String>{"k", "v", "flag", "rk", "name", "rv"});
        CHECK(i64_col(out, "rk") == dstd::Vector<I>{2, 2, 2, 2});
        DataFrame outer = dftu_utils_test::ok(
            make_left().join(right, {"k"}, {"rk"}, JoinHow::Outer));
        CHECK(i64_col(outer, "k") ==
              dstd::Vector<I>{1, 2, 2, 3, 2, 2, NI, NI, NI});
        CHECK(i64_col(outer, "rk") ==
              dstd::Vector<I>{NI, 2, 2, NI, 2, 2, NI, 4, NI});
    }

    TEST_CASE("custom suffix") {
        DataFrame out = dftu_utils_test::ok(
            make_left().join(make_right(), {"k"}, JoinHow::Inner, "_r"));
        CHECK(out.names ==
              dstd::Vector<dstd::String>{"k", "v", "flag", "name", "v_r"});
    }

    TEST_CASE("composite key over int and string") {
        DataFrame l;
        l.names = {"a", "s", "x"};
        l.columns.push_back(i64s({1, 1, 2}));
        l.columns.push_back(Series::strings({"p", "q", "p"}));
        l.columns.push_back(i64s({7, 8, 9}));
        DataFrame r;
        r.names = {"a", "s", "y"};
        r.columns.push_back(i64s({1, 2, 1}));
        r.columns.push_back(Series::strings({"q", "p", "zz"}));
        r.columns.push_back(i64s({100, 200, 300}));
        DataFrame out = dftu_utils_test::ok(l.join(r, {"a", "s"}));
        CHECK(out.names == dstd::Vector<dstd::String>{"a", "s", "x", "y"});
        CHECK(i64_col(out, "x") == dstd::Vector<I>{8, 9});
        CHECK(i64_col(out, "y") == dstd::Vector<I>{100, 200});
    }

    TEST_CASE("refusals name the problem") {
        DataFrame l = make_left();
        DataFrame r = make_right();
        using dftracer::utils::common::ErrorCode;
        dftu_utils_test::expect_error(l.join(r, {"nope"}), ErrorCode::NotFound);
        dftu_utils_test::expect_error(l.join(r, {"k"}, {"nope"}),
                                      ErrorCode::NotFound);
        dftu_utils_test::expect_error(l.join(r, {}),
                                      ErrorCode::InvalidArgument);
        dftu_utils_test::expect_error(l.join(r, {"k"}, {"k", "name"}),
                                      ErrorCode::InvalidArgument);
        dftu_utils_test::expect_error(l.join(r, {"k"}, {"name"}),
                                      ErrorCode::InvalidArgument);
        dftu_utils_test::expect_error(
            l.join(r, {"k"}, static_cast<JoinHow>(99)),
            ErrorCode::InvalidArgument);
    }

    TEST_CASE("lazy parity with eager under one and many morsels") {
        for (JoinHow how :
             {JoinHow::Inner, JoinHow::Left, JoinHow::Right, JoinHow::Outer,
              JoinHow::Semi, JoinHow::Anti, JoinHow::Cross}) {
            INFO("how " << static_cast<int>(how));
            DataFrame eager = dftu_utils_test::ok(make_left().join(
                make_right(),
                how == JoinHow::Cross ? dstd::Vector<dstd::String>{}
                                      : dstd::Vector<dstd::String>{"k"},
                how));
            check_same(lazy_join(how, 0), eager);
            check_same(lazy_join(how, 2), eager);
            check_same(lazy_join(how, 1), eager);
        }
    }

    TEST_CASE("lazy schema, output_schema and explain") {
        LazyFrame left =
            LazyFrame::scan(dstd::make_shared<InMemorySource>(make_left()));
        LazyFrame right =
            LazyFrame::scan(dstd::make_shared<InMemorySource>(make_right()));
        LazyFrame plan = left.join(right, {"k"}, JoinHow::Left);
        CHECK(plan.schema() ==
              dstd::Vector<dstd::String>{"k", "v", "flag", "name", "v_right"});
        Schema s = plan.output_schema();
        REQUIRE(s.fields.size() == 5);
        CHECK(s.fields[0].type.id == TypeId::Int64);
        CHECK(s.fields[2].type.id == TypeId::Bool);
        CHECK(s.fields[3].type.id == TypeId::String);
        CHECK(s.fields[4].type.id == TypeId::Int64);
        CHECK(plan.explain().find("join left [k] = [k]") != dstd::String::npos);
        DataFrame out = run(plan.collect());
        CHECK(out.names == plan.schema());
        LazyFrame semi = left.join(right, {"k"}, JoinHow::Semi);
        CHECK(semi.schema() == dstd::Vector<dstd::String>{"k", "v", "flag"});
    }

    TEST_CASE("a filter after the join is not hoisted across it") {
        LazyFrame left =
            LazyFrame::scan(dstd::make_shared<InMemorySource>(make_left()));
        LazyFrame right =
            LazyFrame::scan(dstd::make_shared<InMemorySource>(make_right()));
        // col(4) is v_right, which only exists after the join.
        LazyFrame plan =
            left.join(right, {"k"}).filter(col(4) > std::int64_t{210});
        dstd::String plan_text = plan.explain();
        CHECK(plan_text.find("join") < plan_text.find("filter"));
        DataFrame out = run(plan.collect());
        CHECK(i64_col(out, "v_right") == dstd::Vector<I>{220, 220});
    }

    TEST_CASE("joining a plan whose right side is itself lazy work") {
        LazyFrame left =
            LazyFrame::scan(dstd::make_shared<InMemorySource>(make_left()));
        LazyFrame right =
            LazyFrame::scan(dstd::make_shared<InMemorySource>(make_right()))
                .filter(col(2) > std::int64_t{210})
                .select({"k", "name"});
        DataFrame out = run(left.join(right, {"k"}).collect());
        CHECK(out.names ==
              dstd::Vector<dstd::String>{"k", "v", "flag", "name"});
        CHECK(str_col(out, "name") == dstd::Vector<S>{"bb", "bb"});
        CHECK(i64_col(out, "v") == dstd::Vector<I>{20, 40});
    }

    TEST_CASE("outer join with an empty left stream still emits right rows") {
        LazyFrame left =
            LazyFrame::scan(dstd::make_shared<InMemorySource>(make_left()))
                .filter(col(1) > std::int64_t{1000});
        LazyFrame right =
            LazyFrame::scan(dstd::make_shared<InMemorySource>(make_right()));
        DataFrame out = run(left.join(right, {"k"}, JoinHow::Outer).collect(2));
        CHECK(out.names ==
              dstd::Vector<dstd::String>{"k", "v", "flag", "name", "v_right"});
        CHECK(i64_col(out, "k") == dstd::Vector<I>{2, 4, 2, NI});
        CHECK(i64_col(out, "v") == dstd::Vector<I>{NI, NI, NI, NI});
        CHECK(bool_col(out, "flag") == dstd::Vector<B>{NB, NB, NB, NB});
        CHECK(str_col(out, "name") == dstd::Vector<S>{"b", "d", "bb", "n"});
    }

    TEST_CASE(
        "compare_agg: outer join on the keys, l_/r_ metrics, delta and "
        "pct") {
        DataFrame base;
        base.names = {"k", "n"};
        base.columns.push_back(i64s({2, 1}));
        base.columns.push_back(i64s({10, 20}));
        DataFrame variant;
        variant.names = {"k", "n"};
        variant.columns.push_back(i64s({1, 3}));
        variant.columns.push_back(i64s({15, 7}));
        DataFrame out = dftu_utils_test::ok(base.compare_agg(variant, 1));
        CHECK(out.names == dstd::Vector<dstd::String>{"k", "l_n", "r_n",
                                                      "delta_n", "pct_n"});
        CHECK(i64_col(out, "k") == dstd::Vector<I>{1, 2, 3});
        CHECK(i64_col(out, "l_n") == dstd::Vector<I>{20, 10, NI});
        CHECK(i64_col(out, "r_n") == dstd::Vector<I>{15, NI, 7});
        CHECK(i64_col(out, "delta_n") == dstd::Vector<I>{-5, NI, NI});
        Series pct = out.column("pct_n").materialize();
        REQUIRE(pct.type() == TypeId::Float64);
        CHECK(pct.data<double>()[0] == doctest::Approx(-25.0));
        CHECK(pct.is_null(1));
        CHECK(pct.is_null(2));
        using dftracer::utils::common::ErrorCode;
        dftu_utils_test::expect_error(base.compare_agg(variant, 0),
                                      ErrorCode::InvalidArgument);
        dftu_utils_test::expect_error(base.compare_agg(variant, 3),
                                      ErrorCode::InvalidArgument);
        dftu_utils_test::expect_error(
            base.compare_agg(dftu_utils_test::ok(variant.rename({"j", "n"})),
                             1),
            ErrorCode::InvalidArgument);
    }

    TEST_CASE(
        "lookup: each left row once, keys by value, fills and conflicts") {
        // left: k = [1, 2, 3, 1, null] (Int64), a = null or `a`
        auto left = [](const dstd::Vector<dstd::String>& a = {}) {
            DataFrame df;
            df.names = {"k", "a"};
            df.columns.push_back(i64s({1, 2, 3, 1, 0}, {0x0f}));
            df.columns.push_back(a.empty() ? Series::nulls(TypeId::String, 5)
                                           : Series::strings(a));
            return df;
        };
        // right: k = [1.0, 2.0, 2.0] (Float64), a, b = [7, 8, 8]
        auto right = [](const dstd::Vector<dstd::String>& a) {
            DataFrame df;
            df.names = {"k", "a", "b"};
            const double rk[] = {1.0, 2.0, 2.0};
            df.columns.push_back(Series::flat_f64(rk, 3));
            df.columns.push_back(Series::strings(a));
            df.columns.push_back(i64s({7, 8, 8}));
            return df;
        };
        const dstd::Vector<dstd::String> xyy{"x", "y", "y"};

        DataFrame out = dftu_utils_test::ok(
            left().join(right(xyy), {"k"}, {"k"}, JoinHow::Lookup));
        CHECK(out.names == dstd::Vector<dstd::String>{"k", "a", "b"});
        CHECK(i64_col(out, "k") == dstd::Vector<I>{1, 2, 3, 1, NI});
        CHECK(str_col(out, "a") == dstd::Vector<S>{"x", "y", NS, "x", NS});
        CHECK(i64_col(out, "b") == dstd::Vector<I>{7, 8, NI, 7, NI});

        LazyFrame ll =
            LazyFrame::scan(dstd::make_shared<InMemorySource>(left()));
        LazyFrame rl =
            LazyFrame::scan(dstd::make_shared<InMemorySource>(right(xyy)));
        for (std::int64_t morsel : {0, 1, 2})
            check_same(run(ll.join(rl, {"k"}, JoinHow::Lookup).collect(morsel)),
                       out);
        CHECK(ll.join(rl, {"k"}, JoinHow::Lookup).schema() == out.names);

        const auto conflict =
            left().join(right({"x", "y", "z"}), {"k"}, {"k"}, JoinHow::Lookup);
        dftu_utils_test::expect_error(
            conflict, dftracer::utils::common::ErrorCode::InvalidArgument);
        const dstd::String what = conflict.error().c_str();
        CHECK(what.find("key 2") != dstd::String::npos);
        CHECK(what.find("'a'") != dstd::String::npos);
        const auto fills = left({"p", "q", "r", "s", "t"})
                               .join(right(xyy), {"k"}, {"k"}, JoinHow::Lookup);
        dftu_utils_test::expect_error(
            fills, dftracer::utils::common::ErrorCode::InvalidArgument);
        CHECK(dstd::String(fills.error().c_str()).find("fills 'a'") !=
              dstd::String::npos);

        // Strings never equal numbers.
        DataFrame text;
        text.names = {"k"};
        text.columns.push_back(Series::strings({"1", "2"}));
        DataFrame none = dftu_utils_test::ok(
            text.join(right(xyy), {"k"}, {"k"}, JoinHow::Lookup));
        CHECK(str_col(none, "a") == dstd::Vector<S>{NS, NS});
    }

    TEST_CASE("nest: every match as a list of structs, empty when none") {
        auto keys = [] {
            DataFrame df;
            df.names = {"k"};
            df.columns.push_back(i64s({2, 5, 0}, {0x03}));
            return df;
        };
        DataFrame left = keys();
        DataFrame out = dftu_utils_test::ok(
            left.join(make_right(), {"k"}, {"k"}, JoinHow::Nest, "m"));
        CHECK(out.names == dstd::Vector<dstd::String>{"k", "m"});
        const Series m = out.column("m").materialize();
        REQUIRE(m.type() == TypeId::List);
        const std::int32_t* off = m.offsets();
        CHECK(off[1] - off[0] == 2);
        CHECK(off[2] - off[1] == 0);
        CHECK(off[3] - off[2] == 0);
        CHECK_FALSE(m.is_null(1));
        const Series rows = m.child(0);
        REQUIRE(rows.type() == TypeId::Struct);
        CHECK(rows.field_name(1) == "name");
        CHECK(dstd::String(rows.child(1).materialize().string_at(0)) == "b");
        CHECK(dstd::String(rows.child(1).materialize().string_at(1)) == "bb");
        dftu_utils_test::expect_error(
            left.join(make_right(), {"k"}, {"k"}, JoinHow::Nest, ""),
            dftracer::utils::common::ErrorCode::InvalidArgument);
        LazyFrame ll =
            LazyFrame::scan(dstd::make_shared<InMemorySource>(keys()));
        LazyFrame rl =
            LazyFrame::scan(dstd::make_shared<InMemorySource>(make_right()));
        const DataFrame lazy =
            run(ll.join(rl, {"k"}, JoinHow::Nest, "m").collect(1));
        CHECK(lazy.names == out.names);
        CHECK(lazy.num_rows() == 3);
    }

    TEST_CASE("C ABI: eager and lazy join agree with the C++ call") {
        DataFrame l = make_left();
        DataFrame r = make_right();
        dftu_dataframe* lh = dftu_dataframe_new(
            dstd::Vector<const char*>{"k", "v", "flag"}.data(),
            dstd::Vector<dftu_series*>{l.columns[0].share().release(),
                                       l.columns[1].share().release(),
                                       l.columns[2].share().release()}
                .data(),
            3);
        dftu_dataframe* rh = dftu_dataframe_new(
            dstd::Vector<const char*>{"k", "name", "v"}.data(),
            dstd::Vector<dftu_series*>{r.columns[0].share().release(),
                                       r.columns[1].share().release(),
                                       r.columns[2].share().release()}
                .data(),
            3);
        REQUIRE(lh);
        REQUIRE(rh);
        const char* on[] = {"k"};
        dftu_dataframe* joined =
            dftu_dataframe_join(lh, rh, on, on, 1, DFTU_JOIN_LEFT, nullptr, 0);
        REQUIRE(joined);
        CHECK(dftu_dataframe_num_rows(joined) == 7);
        CHECK(dftu_dataframe_num_columns(joined) == 5);
        CHECK(dstd::String(dftu_dataframe_column_name(joined, 4)) == "v_right");
        CHECK(dftu_dataframe_join(lh, rh, on, on, 0, DFTU_JOIN_LEFT, nullptr,
                                  0) == nullptr);
        CHECK(dftu_dataframe_join(lh, rh, on, on, 1,
                                  static_cast<dftu_join_how>(9), nullptr,
                                  0) == nullptr);
        const char* bad[] = {"nope"};
        CHECK(dftu_dataframe_join(lh, rh, bad, on, 1, DFTU_JOIN_LEFT, nullptr,
                                  0) == nullptr);

        dftu_lazyframe* ll = dftu_dataframe_lazy(lh);
        dftu_lazyframe* rl = dftu_dataframe_lazy(rh);
        REQUIRE(ll);
        REQUIRE(rl);
        dftu_lazyframe* jl =
            dftu_lazyframe_join(ll, rl, on, on, 1, DFTU_JOIN_LEFT, "_r", 0);
        REQUIRE(jl);
        dftu_dataframe* collected = dftu_lazyframe_collect(jl, 0);
        REQUIRE(collected);
        CHECK(dftu_dataframe_num_rows(collected) == 7);
        CHECK(dstd::String(dftu_dataframe_column_name(collected, 4)) == "v_r");
        CHECK(dftu_lazyframe_join(ll, rl, on, on, 0, DFTU_JOIN_LEFT, nullptr,
                                  0) == nullptr);
        CHECK(dftu_lazyframe_join(ll, nullptr, on, on, 1, DFTU_JOIN_LEFT,
                                  nullptr, 0) == nullptr);
        dftu_dataframe_free(collected);
        dftu_lazyframe_free(jl);
        dftu_lazyframe_free(rl);
        dftu_lazyframe_free(ll);
        dftu_dataframe_free(joined);
        dftu_dataframe_free(rh);
        dftu_dataframe_free(lh);
    }
}
