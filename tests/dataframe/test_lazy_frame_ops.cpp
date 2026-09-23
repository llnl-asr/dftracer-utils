// Lazy unnest, compare_agg and the generic frame_op plan step: schema, explain,
// parity with the eager op under morsels, the C ABI and the registry rows.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using dftracer::utils::coro::CoroTask;
using dftracer::utils::dataframe::col;
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

Series i64s(const std::vector<std::int64_t>& v) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()));
}

using I = std::optional<std::int64_t>;
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

// pid = [1, 2, 3], tk = [[{read, 5}, {write, 7}], [], [{open, 9}]]
DataFrame make_nested() {
    std::int64_t counts[3] = {5, 7, 9};
    std::vector<Series> fields;
    fields.push_back(Series::strings({"read", "write", "open"}));
    fields.push_back(Series::flat_i64(counts, 3));
    DataFrame df;
    df.names = {"pid", "tk"};
    df.columns.push_back(i64s({1, 2, 3}));
    df.columns.push_back(Series::list(
        {0, 2, 2, 3}, Series::structs({"value", "count"}, std::move(fields))));
    return df;
}

DataFrame make_base() {
    DataFrame df;
    df.names = {"k", "n"};
    df.columns.push_back(i64s({2, 1}));
    df.columns.push_back(i64s({10, 20}));
    return df;
}

DataFrame make_variant() {
    DataFrame df;
    df.names = {"k", "n"};
    df.columns.push_back(i64s({1, 3}));
    df.columns.push_back(i64s({15, 7}));
    return df;
}

LazyFrame scan(DataFrame df) {
    return LazyFrame::scan(std::make_shared<InMemorySource>(std::move(df)));
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

TEST_SUITE("lazy frame ops") {
    TEST_CASE("unnest: schema names the struct fields, streams per morsel") {
        LazyFrame plan = scan(make_nested()).unnest("tk");
        CHECK(plan.schema() ==
              std::vector<std::string>{"pid", "value", "count"});
        Schema s = plan.output_schema();
        REQUIRE(s.fields.size() == 3);
        CHECK(s.fields[1].type.id == TypeId::String);
        CHECK(s.fields[2].type.id == TypeId::Int64);
        CHECK(plan.explain().find("unnest tk") != std::string::npos);
        DataFrame eager = make_nested().unnest("tk");
        for (std::int64_t morsel : std::vector<std::int64_t>{0, 1}) {
            DataFrame out =
                run(scan(make_nested()).unnest("tk").collect(morsel));
            CHECK(out.names == eager.names);
            CHECK(i64_col(out, "pid") == i64_col(eager, "pid"));
            CHECK(i64_col(out, "count") == i64_col(eager, "count"));
        }
        DataFrame kept = run(scan(make_nested()).unnest("tk", true).collect(1));
        CHECK(i64_col(kept, "pid") == std::vector<I>{1, 1, 2, 3});
        CHECK(i64_col(kept, "count") == std::vector<I>{5, 7, NI, 9});
        // A filter on a flattened field resolves by the unnest's schema.
        DataFrame big = run(scan(make_nested())
                                .unnest("tk")
                                .filter(col(2) > std::int64_t{6})
                                .collect());
        CHECK(i64_col(big, "count") == std::vector<I>{7, 9});
        CHECK_THROWS_AS(scan(make_nested()).unnest("nope"), std::out_of_range);
        CHECK_THROWS_AS(scan(make_nested()).unnest("pid"),
                        std::invalid_argument);
    }

    TEST_CASE("compare_agg: data-dependent schema, matches the eager op") {
        LazyFrame plan = scan(make_base()).compare_agg(scan(make_variant()), 1);
        CHECK(plan.schema().empty());
        CHECK(plan.explain().find("frame_op dftu.frame.compare_agg") !=
              std::string::npos);
        DataFrame out = run(plan.collect(1));
        DataFrame eager = make_base().compare_agg(make_variant(), 1);
        CHECK(out.names == eager.names);
        CHECK(i64_col(out, "k") == std::vector<I>{1, 2, 3});
        CHECK(i64_col(out, "delta_n") == std::vector<I>{-5, NI, NI});
        CHECK_THROWS_AS(scan(make_base()).compare_agg(scan(make_variant()), 0),
                        std::invalid_argument);
        // A key mismatch is a collect-time error, not a build-time one.
        LazyFrame bad =
            scan(make_base())
                .compare_agg(scan(make_variant().rename({"j", "n"})), 1);
        CHECK_THROWS(run(bad.collect()));
    }

    TEST_CASE(
        "frame_op: any registry table op as a plan step, operands "
        "owned") {
        // The operands go out of scope before the plan runs: the plan must
        // hold its own copies.
        LazyFrame plan = [] {
            std::string name = "n";
            OpArgs args;
            args.str(1, name).i64(2, 1).i32(3, 1);
            return scan(make_base()).frame_op("dftu.frame.topk", args);
        }();
        CHECK(plan.explain().find("frame_op dftu.frame.topk") !=
              std::string::npos);
        DataFrame out = run(plan.collect(1));
        CHECK(i64_col(out, "n") == std::vector<I>{20});

        OpArgs none;
        CHECK_THROWS_WITH_AS(
            scan(make_base()).frame_op("dftu.frame.nope", none),
            "lazy frame op 'dftu.frame.nope': no op "
            "registered under this name",
            std::invalid_argument);
        CHECK_THROWS_AS(scan(make_base()).frame_op("dftu.series.add", none),
                        std::invalid_argument);
        CHECK_THROWS_AS(scan(make_base()).frame_op("dftu.frame.union", none),
                        std::invalid_argument);
        CHECK_THROWS_AS(
            scan(make_base())
                .frame_op("dftu.frame.union", none,
                          {scan(make_variant()), scan(make_variant())}),
            std::invalid_argument);
        DataFrame u =
            run(scan(make_base())
                    .frame_op("dftu.frame.union", none, {scan(make_variant())})
                    .collect());
        CHECK(u.num_rows() == 4);
    }

    TEST_CASE(
        "reduce: one aggregate over every eligible column, whole frame "
        "or per key") {
        using dftracer::utils::dataframe::Agg;
        using dftracer::utils::dataframe::GroupAgg;
        DataFrame df;
        df.names = {"k", "s", "a", "b"};
        double b[4] = {2.0, 4.0, 6.5, 1.5};
        df.columns.push_back(Series::strings({"x", "y", "x", "y"}));
        df.columns.push_back(Series::strings({"p", "q", "r", "s"}));
        df.columns.push_back(i64s({1, 2, 3, 4}));
        df.columns.push_back(Series::flat_f64(b, 4));

        DataFrame sum = df.reduce(Agg::Sum);
        CHECK(sum.names == std::vector<std::string>{"a", "b"});
        REQUIRE(sum.num_rows() == 1);
        CHECK(i64_col(sum, "a") == std::vector<I>{10});
        CHECK(sum.column("b").materialize().data<double>()[0] ==
              doctest::Approx(14.0));
        DataFrame present = df.reduce(Agg::CountValid);
        CHECK(present.names == std::vector<std::string>{"k", "s", "a", "b"});
        CHECK(i64_col(present, "s") == std::vector<I>{4});
        // The grouped broadcast lives on the group-by object.
        DataFrame by_k = df.group_by({"k"}).max();
        CHECK(by_k.names == std::vector<std::string>{"k", "a", "b"});
        CHECK(i64_col(by_k, "a") == std::vector<I>{3, 4});
        DataFrame size = df.group_by({"k"}).size();
        CHECK(size.names == std::vector<std::string>{"k", "size"});
        CHECK(i64_col(size, "size") == std::vector<I>{2, 2});
        CHECK_THROWS_AS(df.reduce(Agg::Corr), std::invalid_argument);
        CHECK_THROWS_AS(df.group_by({"k"}).reduce(Agg::Corr),
                        std::invalid_argument);
        CHECK_THROWS_AS(df.group_by({"nope"}), std::out_of_range);

        // The general form: any spec, one row, or per group.
        std::vector<GroupAgg> specs;
        specs.push_back(GroupAgg{Agg::Corr, "b", "c", 0.0, "a"});
        specs.push_back(GroupAgg{Agg::Count, "", "n"});
        DataFrame gen = df.reduce(specs);
        CHECK(gen.names == std::vector<std::string>{"c", "n"});
        CHECK(i64_col(gen, "n") == std::vector<I>{4});
        CHECK(i64_col(df.group_by().agg(specs), "n") == std::vector<I>{4});
        CHECK(i64_col(df.group_by({"k"}).agg(specs), "n") ==
              std::vector<I>{2, 2});

        // Lazy: a streaming group-by with the same broadcast.
        auto scan_df = [&] {
            DataFrame c;
            c.names = df.names;
            for (const Series& s : df.columns) c.columns.push_back(s.share());
            return scan(std::move(c));
        };
        LazyFrame plan = scan_df().reduce(Agg::Sum);
        CHECK(plan.schema() == std::vector<std::string>{"a", "b"});
        CHECK(plan.explain().find("group_by") != std::string::npos);
        DataFrame lz = run(plan.collect(1));
        CHECK(i64_col(lz, "a") == std::vector<I>{10});
        DataFrame lz_k = run(scan_df().group_by({"k"}).max().collect(1));
        CHECK(lz_k.names == std::vector<std::string>{"k", "a", "b"});
        CHECK(i64_col(lz_k, "a") == std::vector<I>{3, 4});
        DataFrame lz_size = run(scan_df().group_by({"k"}).size().collect(1));
        CHECK(i64_col(lz_size, "size") == std::vector<I>{2, 2});
        CHECK_THROWS_AS(scan_df().group_by({"nope"}), std::out_of_range);
        CHECK_THROWS_AS(scan_df().reduce_specs(Agg::Sum, {"nope"}),
                        std::out_of_range);

        dftu_dataframe* h = to_abi(df);
        REQUIRE(h);
        dftu_dataframe* eager = dftu_dataframe_reduce(h, DFTU_AGG_MAX);
        REQUIRE(eager);
        CHECK(dftu_dataframe_num_rows(eager) == 1);
        CHECK(dftu_dataframe_num_columns(eager) == 2);
        CHECK(dftu_dataframe_reduce(h, DFTU_AGG_CORR) == nullptr);
        CHECK(dftu_dataframe_reduce(h, 999) == nullptr);
        const dftu_dataframe* fin[1] = {h};
        OpArgs arg;
        arg.i32(1, DFTU_AGG_SUM);
        dftu_dataframe* via_op =
            dftu_op_run_frame(dftu_op_find("dftu.frame.reduce"), fin, 1, arg);
        REQUIRE(via_op);
        CHECK(dftu_dataframe_num_rows(via_op) == 1);
        CHECK(dftu_dataframe_num_columns(via_op) == 2);
        dftu_lazyframe* lh = dftu_dataframe_lazy(h);
        const dftu_lazyframe* lin[1] = {lh};
        OpArgs larg;
        larg.i32(1, DFTU_AGG_MAX);
        dftu_lazyframe* lazy_op =
            dftu_op_run_lazy(dftu_op_find("dftu.lazy.reduce"), lin, 1, larg);
        REQUIRE(lazy_op);
        dftu_dataframe* lazy_out = dftu_lazyframe_collect(lazy_op, 0);
        REQUIRE(lazy_out);
        CHECK(dftu_dataframe_num_rows(lazy_out) == 1);
        CHECK(dftu_dataframe_num_columns(lazy_out) == 2);
        dftu_dataframe_free(lazy_out);
        dftu_lazyframe_free(lazy_op);
        dftu_lazyframe_free(lh);
        dftu_dataframe_free(via_op);
        dftu_dataframe_free(eager);
        dftu_dataframe_free(h);
    }

    TEST_CASE("a group with no present value has no min, max or moment") {
        using dftracer::utils::dataframe::Agg;
        // k = [x, y, x], a = [1, null, 3]: group y has no value of a.
        const std::int64_t av[3] = {1, 0, 3};
        const std::uint8_t valid[1] = {0x05};
        DataFrame df;
        df.names = {"k", "a"};
        df.columns.push_back(Series::strings({"x", "y", "x"}));
        df.columns.push_back(Series::flat_i64(av, 3, valid));
        CHECK(i64_col(df.group_by({"k"}).min(), "a") == std::vector<I>{1, NI});
        CHECK(i64_col(df.group_by({"k"}).max(), "a") == std::vector<I>{3, NI});
        CHECK(i64_col(df.group_by({"k"}).sum(), "a") == std::vector<I>{4, 0});
        CHECK(i64_col(df.group_by({"k"}).count(), "a") == std::vector<I>{2, 0});
        Series mean = df.group_by({"k"}).mean().column("a").materialize();
        CHECK(mean.data<double>()[0] == doctest::Approx(2.0));
        CHECK(mean.is_null(1));
        // A sample variance needs two values: x has them, y has none.
        Series var = df.group_by({"k"}).var().column("a").materialize();
        CHECK(var.data<double>()[0] == doctest::Approx(2.0));
        CHECK(var.is_null(1));
        // A Bool column casts to 0 / 1.
        Series present = df.column("a").materialize();
        Series bits = Series{dftu_series_valid_mask(present.handle())};
        Series ones = bits.cast(TypeId::Int64);
        REQUIRE(ones.valid());
        CHECK(i64_col(DataFrame{{"o"},
                                [&] {
                                    std::vector<Series> c;
                                    c.push_back(std::move(ones));
                                    return c;
                                }()},
                      "o") == std::vector<I>{1, 0, 1});
    }

    TEST_CASE("unique(subset): keyed on the named columns, first row kept") {
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(i64s({1, 1, 2, 2, 3}));
        df.columns.push_back(i64s({1, 2, 3, 3, 4}));
        DataFrame all = df.unique();
        CHECK(i64_col(all, "v") == std::vector<I>{1, 2, 3, 4});
        DataFrame by_k = df.unique({"k"});
        CHECK(i64_col(by_k, "k") == std::vector<I>{1, 2, 3});
        CHECK(i64_col(by_k, "v") == std::vector<I>{1, 3, 4});
        CHECK_THROWS_AS(df.unique({"nope"}), std::out_of_range);

        auto scan_df = [&] {
            DataFrame c;
            c.names = df.names;
            for (const Series& s : df.columns) c.columns.push_back(s.share());
            return scan(std::move(c));
        };
        LazyFrame plan = scan_df().unique({"k"});
        CHECK(plan.explain().find("unique [k]") != std::string::npos);
        for (std::int64_t morsel : std::vector<std::int64_t>{0, 1, 2}) {
            DataFrame out = run(scan_df().unique({"k"}).collect(morsel));
            CHECK(i64_col(out, "k") == std::vector<I>{1, 2, 3});
            CHECK(i64_col(out, "v") == std::vector<I>{1, 3, 4});
        }
        // A tiny budget forces the spill path; the keys still honour the
        // subset.
        DataFrame spilled =
            run(scan_df().memory_budget(1).unique({"k"}).collect(1));
        CHECK(i64_col(spilled, "k") == std::vector<I>{1, 2, 3});
        CHECK(i64_col(spilled, "v") == std::vector<I>{1, 3, 4});
        CHECK_THROWS_AS(scan_df().unique({"nope"}), std::out_of_range);

        dftu_dataframe* h = to_abi(df);
        REQUIRE(h);
        const char* keys[1] = {"k"};
        dftu_dataframe* eager = dftu_dataframe_unique_by(h, keys, 1);
        REQUIRE(eager);
        CHECK(dftu_dataframe_num_rows(eager) == 3);
        CHECK(dftu_dataframe_unique_by(h, keys, 0) == nullptr);
        dftu_lazyframe* lh = dftu_dataframe_lazy(h);
        dftu_lazyframe* lazy = dftu_lazyframe_unique_by(lh, keys, 1);
        REQUIRE(lazy);
        dftu_dataframe* lazy_out = dftu_lazyframe_collect(lazy, 0);
        REQUIRE(lazy_out);
        CHECK(dftu_dataframe_num_rows(lazy_out) == 3);
        const dftu_lazyframe* in1[1] = {lh};
        OpArgs arg;
        arg.strlist(1, keys, 1);
        dftu_lazyframe* via_op =
            dftu_op_run_lazy(dftu_op_find("dftu.lazy.unique_by"), in1, 1, arg);
        REQUIRE(via_op);
        const dftu_dataframe* fin[1] = {h};
        dftu_dataframe* via_frame_op = dftu_op_run_frame(
            dftu_op_find("dftu.frame.unique_by"), fin, 1, arg);
        REQUIRE(via_frame_op);
        CHECK(dftu_dataframe_num_rows(via_frame_op) == 3);
        dftu_dataframe_free(via_frame_op);
        dftu_lazyframe_free(via_op);
        dftu_dataframe_free(lazy_out);
        dftu_lazyframe_free(lazy);
        dftu_lazyframe_free(lh);
        dftu_dataframe_free(eager);
        dftu_dataframe_free(h);
    }

    TEST_CASE("C ABI and registry rows") {
        dftu_dataframe* b = to_abi(make_base());
        dftu_dataframe* v = to_abi(make_variant());
        dftu_dataframe* n = to_abi(make_nested());
        REQUIRE(b);
        REQUIRE(v);
        REQUIRE(n);
        dftu_lazyframe* lb = dftu_dataframe_lazy(b);
        dftu_lazyframe* lv = dftu_dataframe_lazy(v);
        dftu_lazyframe* ln = dftu_dataframe_lazy(n);

        dftu_lazyframe* un = dftu_lazyframe_unnest(ln, "tk", 1);
        REQUIRE(un);
        dftu_dataframe* un_out = dftu_lazyframe_collect(un, 0);
        REQUIRE(un_out);
        CHECK(dftu_dataframe_num_rows(un_out) == 4);
        CHECK(dftu_dataframe_num_columns(un_out) == 3);
        CHECK(dftu_lazyframe_unnest(ln, "pid", 0) == nullptr);
        dftu_dataframe_free(un_out);
        dftu_lazyframe_free(un);

        dftu_lazyframe* ca = dftu_lazyframe_compare_agg(lb, lv, 1);
        REQUIRE(ca);
        dftu_dataframe* ca_out = dftu_lazyframe_collect(ca, 0);
        REQUIRE(ca_out);
        CHECK(dftu_dataframe_num_rows(ca_out) == 3);
        CHECK(std::string(dftu_dataframe_column_name(ca_out, 4)) == "pct_n");
        CHECK(dftu_lazyframe_compare_agg(lb, lv, 0) == nullptr);
        dftu_dataframe_free(ca_out);
        dftu_lazyframe_free(ca);

        const dftu_lazyframe* others[1] = {lv};
        const char* names[2] = {"k", "n"};
        dftu_lazyframe* fo = dftu_lazyframe_frame_op(
            lb, "dftu.frame.union", others, 1, nullptr, names, 2);
        REQUIRE(fo);
        char* fo_plan = dftu_lazyframe_explain(fo);
        REQUIRE(fo_plan);
        dftu_query_string_free(fo_plan);
        // With the names given, a later op resolves them before collect.
        dftu_lazyframe* sorted = dftu_lazyframe_sort_by(fo, "n", 0);
        REQUIRE(sorted);
        dftu_dataframe* fo_out = dftu_lazyframe_collect(sorted, 0);
        REQUIRE(fo_out);
        CHECK(dftu_dataframe_num_rows(fo_out) == 4);
        CHECK(std::string(dftu_dataframe_column_name(fo_out, 1)) == "n");
        dftu_lazyframe_free(sorted);
        CHECK(dftu_lazyframe_frame_op(lb, "dftu.frame.union", nullptr, 0,
                                      nullptr, nullptr, 0) == nullptr);
        dftu_dataframe_free(fo_out);
        dftu_lazyframe_free(fo);

        const dftu_lazyframe* in1[1] = {ln};
        OpArgs uarg;
        uarg.str(1, "tk").i32(2, 0);
        dftu_lazyframe* via_op =
            dftu_op_run_lazy(dftu_op_find("dftu.lazy.unnest"), in1, 1, uarg);
        REQUIRE(via_op);
        dftu_dataframe* via_out = dftu_lazyframe_collect(via_op, 0);
        REQUIRE(via_out);
        CHECK(dftu_dataframe_num_rows(via_out) == 3);
        dftu_dataframe_free(via_out);
        dftu_lazyframe_free(via_op);

        const dftu_lazyframe* in2[2] = {lb, lv};
        OpArgs carg;
        carg.i64(2, 1);
        dftu_lazyframe* via_ca = dftu_op_run_lazy(
            dftu_op_find("dftu.lazy.compare_agg"), in2, 2, carg);
        REQUIRE(via_ca);
        dftu_dataframe* via_ca_out = dftu_lazyframe_collect(via_ca, 0);
        REQUIRE(via_ca_out);
        CHECK(dftu_dataframe_num_rows(via_ca_out) == 3);
        dftu_dataframe_free(via_ca_out);
        dftu_lazyframe_free(via_ca);

        dftu_lazyframe_free(ln);
        dftu_lazyframe_free(lv);
        dftu_lazyframe_free(lb);
        dftu_dataframe_free(n);
        dftu_dataframe_free(v);
        dftu_dataframe_free(b);
    }

    // k = [x, y, x, y, x], a = [3, 1, null, 2, 3], b = [1, 5, 2, 5, 0.5].
    // Every expected value is pandas 2.x groupby("k").<transform>() over the
    // same frame (pct_change with fill_method=None).
    static DataFrame make_groups() {
        const std::int64_t av[5] = {3, 1, 0, 2, 3};
        const std::uint8_t valid[1] = {0x1b};
        const double bv[5] = {1.0, 5.0, 2.0, 5.0, 0.5};
        DataFrame df;
        df.names = {"k", "a", "b"};
        df.columns.push_back(Series::strings({"x", "y", "x", "y", "x"}));
        df.columns.push_back(Series::flat_i64(av, 5, valid));
        df.columns.push_back(Series::flat_f64(bv, 5));
        return df;
    }

    using D = std::optional<double>;
    const D ND = std::nullopt;

    static std::vector<D> f64_col(const DataFrame& df,
                                  const std::string& name) {
        Series c = df.column(name);
        REQUIRE(c.valid());
        c = c.materialize();
        REQUIRE(c.type() == TypeId::Float64);
        std::vector<D> out;
        for (std::int64_t i = 0; i < c.length(); ++i) {
            if (c.is_null(i))
                out.emplace_back(std::nullopt);
            else
                out.emplace_back(c.data<double>()[i]);
        }
        return out;
    }

    TEST_CASE("group-wise transforms: eager and lazy, pandas values") {
        using dftracer::utils::dataframe::GroupBy;
        using dftracer::utils::dataframe::GroupwiseOp;
        using dftracer::utils::dataframe::LazyGroupBy;
        using dftracer::utils::dataframe::RankMethod;
        const DataFrame df = make_groups();
        GroupBy g = df.group_by({"k"});

        DataFrame cs = g.cumsum();
        CHECK(cs.names == std::vector<std::string>{"a", "b"});
        CHECK(i64_col(cs, "a") == std::vector<I>{3, 1, NI, 3, 6});
        CHECK(f64_col(cs, "b") == std::vector<D>{1.0, 5.0, 3.0, 10.0, 3.5});
        CHECK(i64_col(g.cummax(), "a") == std::vector<I>{3, 1, NI, 2, 3});
        CHECK(f64_col(g.cummax(), "b") ==
              std::vector<D>{1.0, 5.0, 2.0, 5.0, 2.0});
        CHECK(i64_col(g.cummin(), "a") == std::vector<I>{3, 1, NI, 1, 3});
        CHECK(f64_col(g.cummin(), "b") ==
              std::vector<D>{1.0, 5.0, 1.0, 5.0, 0.5});
        DataFrame cc = g.cumcount();
        CHECK(cc.names == std::vector<std::string>{"cumcount"});
        CHECK(i64_col(cc, "cumcount") == std::vector<I>{0, 0, 1, 1, 2});
        CHECK(i64_col(g.ngroup(), "ngroup") == std::vector<I>{0, 1, 0, 1, 0});

        DataFrame sh = g.shift();
        CHECK(sh.names == std::vector<std::string>{"a", "b"});
        CHECK(i64_col(sh, "a") == std::vector<I>{NI, NI, 3, 1, NI});
        CHECK(f64_col(sh, "b") == std::vector<D>{ND, ND, 1.0, 5.0, 2.0});
        CHECK(i64_col(g.shift(-1), "a") == std::vector<I>{NI, 2, 3, NI, NI});
        CHECK(f64_col(g.shift(-1), "b") ==
              std::vector<D>{2.0, 5.0, 0.5, ND, ND});
        CHECK(i64_col(g.diff(), "a") == std::vector<I>{NI, NI, NI, 1, NI});
        CHECK(f64_col(g.diff(), "b") == std::vector<D>{ND, ND, 1.0, 0.0, -1.5});
        CHECK(f64_col(g.pct_change(), "b") ==
              std::vector<D>{ND, ND, 1.0, 0.0, -0.75});

        CHECK(f64_col(g.rank(), "a") == std::vector<D>{1.5, 1.0, ND, 2.0, 1.5});
        CHECK(f64_col(g.rank(), "b") ==
              std::vector<D>{2.0, 1.5, 3.0, 1.5, 1.0});
        CHECK(f64_col(g.rank(RankMethod::Average, false), "b") ==
              std::vector<D>{2.0, 1.5, 1.0, 1.5, 3.0});
        CHECK(f64_col(g.rank(RankMethod::Min), "b") ==
              std::vector<D>{2.0, 1.0, 3.0, 1.0, 1.0});
        CHECK(f64_col(g.rank(RankMethod::Dense), "a") ==
              std::vector<D>{1.0, 1.0, ND, 2.0, 1.0});
        CHECK(f64_col(g.rank(RankMethod::Ordinal), "a") ==
              std::vector<D>{1.0, 1.0, ND, 2.0, 2.0});
        CHECK(f64_col(g.rank(RankMethod::Ordinal, false), "b") ==
              std::vector<D>{2.0, 1.0, 1.0, 2.0, 3.0});
        CHECK(f64_col(g.rank(RankMethod::Max), "b") ==
              std::vector<D>{2.0, 2.0, 3.0, 2.0, 1.0});
        CHECK(f64_col(g.rank(RankMethod::Max, false), "b") ==
              std::vector<D>{2.0, 2.0, 1.0, 2.0, 3.0});
        CHECK(f64_col(g.rank(RankMethod::Max), "a") ==
              std::vector<D>{2.0, 1.0, ND, 2.0, 2.0});

        DataFrame h2 = g.head(2);
        CHECK(h2.names == std::vector<std::string>{"k", "a", "b"});
        CHECK(f64_col(h2, "b") == std::vector<D>{1.0, 5.0, 2.0, 5.0});
        CHECK(f64_col(g.tail(1), "b") == std::vector<D>{5.0, 0.5});
        CHECK(f64_col(g.nth(1), "b") == std::vector<D>{2.0, 5.0});
        CHECK(f64_col(g.nth(-1), "b") == std::vector<D>{5.0, 0.5});

        // A trailing window within the group, null until it holds `n`
        // present values (a null inside the window counts for nothing).
        CHECK(f64_col(g.rolling_sum(2), "b") ==
              std::vector<D>{ND, ND, 3.0, 10.0, 2.5});
        CHECK(i64_col(g.rolling_sum(2), "a") ==
              std::vector<I>{NI, NI, NI, 3, NI});
        CHECK(f64_col(g.rolling_mean(2), "b") ==
              std::vector<D>{ND, ND, 1.5, 5.0, 1.25});
        CHECK(f64_col(g.rolling_max(2), "b") ==
              std::vector<D>{ND, ND, 2.0, 5.0, 2.0});
        CHECK(f64_col(g.rolling_min(2), "b") ==
              std::vector<D>{ND, ND, 1.0, 5.0, 0.5});
        CHECK(i64_col(g.rolling_sum(1), "a") == std::vector<I>{3, 1, NI, 2, 3});
        CHECK_THROWS_AS(g.rolling_sum(0), std::invalid_argument);

        // The group-wise fills: a null before the group's first present value
        // stays null under ffill, after its last under bfill.
        DataFrame holes;
        holes.names = {"k", "a"};
        holes.columns.push_back(Series::strings({"x", "y", "x", "x", "y"}));
        const std::int64_t hv[5] = {0, 1, 2, 0, 0};
        const std::uint8_t hvalid[1] = {0x06};
        holes.columns.push_back(Series::flat_i64(hv, 5, hvalid));
        GroupBy hg = holes.group_by({"k"});
        CHECK(i64_col(hg.ffill(), "a") == std::vector<I>{NI, 1, 2, 2, 1});
        CHECK(i64_col(hg.bfill(), "a") == std::vector<I>{2, 1, 2, NI, NI});

        // The lazy object builds the same plan; the eager one collects it.
        LazyGroupBy lg = df.lazy().group_by({"k"});
        CHECK(i64_col(run(lg.cumsum().collect()), "a") ==
              std::vector<I>{3, 1, NI, 3, 6});
        CHECK(f64_col(run(lg.rolling_mean(2).collect()), "b") ==
              std::vector<D>{ND, ND, 1.5, 5.0, 1.25});
        CHECK(i64_col(run(holes.lazy().group_by({"k"}).ffill().collect()),
                      "a") == std::vector<I>{NI, 1, 2, 2, 1});
        CHECK(f64_col(run(lg.rank().collect()), "a") ==
              std::vector<D>{1.5, 1.0, ND, 2.0, 1.5});
        CHECK(f64_col(run(lg.tail(1).collect()), "b") ==
              std::vector<D>{5.0, 0.5});
        CHECK(run(lg.transform(GroupwiseOp::Head, 2).collect()).num_rows() ==
              4);

        // No keys: the whole frame is one group; ngroup has nothing to number.
        CHECK(i64_col(df.group_by({}).cumsum(), "a") ==
              std::vector<I>{3, 4, NI, 6, 9});
        CHECK_THROWS_AS(df.group_by({}).ngroup(), std::invalid_argument);
        DataFrame keys_only;
        keys_only.names = {"k"};
        keys_only.columns.push_back(Series::strings({"x", "y"}));
        CHECK_THROWS_AS(keys_only.group_by({"k"}).cumsum(),
                        std::invalid_argument);
    }

    TEST_CASE("column_op: a registered column op over one column") {
        using dftracer::utils::dataframe::OpArgs;
        const DataFrame df = make_groups();
        dftu_dataframe* h = to_abi(df);
        REQUIRE(h);
        dftu_scalar zero{};
        zero.kind = DFTU_SCALAR_TAG_I64;
        dftu_scalar two = zero;
        two.value.i = 2;
        // cumsum over b: the other columns pass through.
        dftu_dataframe* cs = dftu_dataframe_column_op(
            h, "b", "dftu.series.cumsum", nullptr, zero, zero, nullptr);
        REQUIRE(cs);
        CHECK(dftu_dataframe_num_columns(cs) == 3);
        dftu_series* b = dftu_dataframe_column(cs, "b");
        REQUIRE(b);
        CHECK(reinterpret_cast<const double*>(dftu_series_data(b))[4] ==
              doctest::Approx(13.5));
        dftu_series_free(b);
        dftu_dataframe_free(cs);
        // A second column operand and the scalar operands in signature order.
        dftu_dataframe* add = dftu_dataframe_column_op(
            h, "b", "dftu.series.add", "b", zero, zero, nullptr);
        REQUIRE(add);
        dftu_series* bb = dftu_dataframe_column(add, "b");
        CHECK(reinterpret_cast<const double*>(dftu_series_data(bb))[1] ==
              doctest::Approx(10.0));
        dftu_series_free(bb);
        dftu_dataframe_free(add);
        dftu_scalar one = zero;
        one.value.i = 1;
        dftu_dataframe* roll = dftu_dataframe_column_op(
            h, "b", "dftu.series.rolling", nullptr, two, one, nullptr);
        REQUIRE(roll);
        dftu_series* rb = dftu_dataframe_column(roll, "b");
        CHECK(dftu_series_is_null(rb, 0));
        CHECK(reinterpret_cast<const double*>(dftu_series_data(rb))[1] ==
              doctest::Approx(3.0));
        dftu_series_free(rb);
        dftu_dataframe_free(roll);
        // Refusals: an absent column or op, an op that is not column ->
        // column, an operand without a source, a length-changing op.
        CHECK(dftu_dataframe_column_op(h, "zz", "dftu.series.cumsum", nullptr,
                                       zero, zero, nullptr) == nullptr);
        CHECK(dftu_dataframe_column_op(h, "b", "dftu.series.nope", nullptr,
                                       zero, zero, nullptr) == nullptr);
        CHECK(dftu_dataframe_column_op(h, "b", "dftu.series.sum", nullptr, zero,
                                       zero, nullptr) == nullptr);
        CHECK(dftu_dataframe_column_op(h, "b", "dftu.series.add", nullptr, zero,
                                       zero, nullptr) == nullptr);
        CHECK(dftu_dataframe_column_op(h, "b", "dftu.series.unique", nullptr,
                                       zero, zero, nullptr) == nullptr);
        dftu_dataframe_free(h);

        // The same as a plan step through the registry row.
        OpArgs a;
        a.str(1, "b").str(2, "dftu.series.cumsum").str(3, "");
        a.scalar(4, dftracer::utils::dataframe::Scalar{zero})
            .scalar(5, dftracer::utils::dataframe::Scalar{zero})
            .str(6, "");
        LazyFrame lazy =
            df.lazy().frame_op("dftu.frame.column_op", a, {}, df.names);
        DataFrame out = run(lazy.collect());
        CHECK(f64_col(out, "b") == std::vector<D>{1.0, 6.0, 8.0, 13.0, 13.5});
    }

    TEST_CASE("group-wise transforms: C ABI and registry rows") {
        dftu_dataframe* h = to_abi(make_groups());
        REQUIRE(h);
        const char* keys[1] = {"k"};
        dftu_dataframe* cs = dftu_dataframe_group_transform(
            h, keys, 1, DFTU_GROUPWISE_CUMSUM, 0, DFTU_RANK_AVERAGE, 1);
        REQUIRE(cs);
        CHECK(dftu_dataframe_num_rows(cs) == 5);
        CHECK(dftu_dataframe_num_columns(cs) == 2);
        dftu_dataframe_free(cs);
        CHECK(dftu_dataframe_group_transform(h, keys, 1, 99, 0, 0, 1) ==
              nullptr);
        CHECK(dftu_dataframe_group_transform(h, keys, 1, DFTU_GROUPWISE_RANK, 0,
                                             7, 1) == nullptr);
        const char* bad[1] = {"nope"};
        CHECK(dftu_dataframe_group_transform(h, bad, 1, DFTU_GROUPWISE_CUMSUM,
                                             0, 0, 1) == nullptr);

        dftu_lazyframe* lf = dftu_dataframe_lazy(h);
        dftu_lazyframe* sh = dftu_lazyframe_group_transform(
            lf, keys, 1, DFTU_GROUPWISE_SHIFT, -1, DFTU_RANK_AVERAGE, 1);
        REQUIRE(sh);
        dftu_dataframe* sh_out = dftu_lazyframe_collect(sh, 0);
        REQUIRE(sh_out);
        CHECK(dftu_dataframe_num_rows(sh_out) == 5);
        CHECK(std::string(dftu_dataframe_column_name(sh_out, 0)) == "a");
        dftu_dataframe_free(sh_out);
        dftu_lazyframe_free(sh);
        CHECK(dftu_lazyframe_group_transform(lf, keys, 1, -1, 0, 0, 1) ==
              nullptr);

        const dftu_op_desc* fop = dftu_op_find("dftu.frame.group_transform");
        const dftu_op_desc* lop = dftu_op_find("dftu.lazy.group_transform");
        REQUIRE(fop);
        REQUIRE(lop);
        OpArgs a;
        a.strlist(1, keys, 1)
            .i32(2, DFTU_GROUPWISE_TAIL)
            .i64(3, 1)
            .i32(4, DFTU_RANK_AVERAGE)
            .i32(5, 1);
        const dftu_dataframe* fin[1] = {h};
        dftu_dataframe* via_f = dftu_op_run_frame(fop, fin, 1, a);
        REQUIRE(via_f);
        CHECK(dftu_dataframe_num_rows(via_f) == 2);
        dftu_dataframe_free(via_f);
        const dftu_lazyframe* lin[1] = {lf};
        dftu_lazyframe* via_l = dftu_op_run_lazy(lop, lin, 1, a);
        REQUIRE(via_l);
        dftu_dataframe* via_l_out = dftu_lazyframe_collect(via_l, 0);
        REQUIRE(via_l_out);
        CHECK(dftu_dataframe_num_rows(via_l_out) == 2);
        dftu_dataframe_free(via_l_out);
        dftu_lazyframe_free(via_l);
        dftu_lazyframe_free(lf);
        dftu_dataframe_free(h);
    }
}
