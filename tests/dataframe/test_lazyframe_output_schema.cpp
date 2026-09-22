#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
// clang-format off
#include <nanoarrow/nanoarrow.h>
#include <dftracer/utils/dataframe/arrow.h>
// clang-format on
#endif

using namespace dftracer::utils::dataframe;

namespace {

DataFrame run(dftracer::utils::coro::CoroTask<DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

// The strongest oracle: what collect() actually produced, as a Schema, so it
// can be compared field-for-field against output_schema()'s claim.
Schema collected_schema(const DataFrame& df) {
    Schema s;
    s.fields.reserve(df.columns.size());
    for (std::size_t i = 0; i < df.columns.size(); ++i)
        s.fields.push_back(Field{df.names[i], df.columns[i].data_type(), true});
    return s;
}

void check_matches_collect(const LazyFrame& lf) {
    Schema claimed = lf.output_schema();
    DataFrame collected = run(lf.collect());
    Schema actual = collected_schema(collected);
    REQUIRE(claimed.fields.size() == actual.fields.size());
    for (std::size_t i = 0; i < claimed.fields.size(); ++i) {
        CAPTURE(i);
        CAPTURE(claimed.fields[i].name);
        CHECK(claimed.fields[i].name == actual.fields[i].name);
        CHECK(claimed.fields[i].type == actual.fields[i].type);
    }
}

Series i64(std::vector<std::int64_t> v) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()));
}
Series f64(std::vector<double> v) {
    return Series::flat_f64(v.data(), static_cast<std::int64_t>(v.size()));
}

std::shared_ptr<const Source> src(DataFrame df) {
    return std::make_shared<const InMemorySource>(std::move(df));
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW

struct BuiltArrow {
    ArrowSchema schema{};
    ArrowArray array{};
    ~BuiltArrow() {
        if (array.release) array.release(&array);
        if (schema.release) schema.release(&schema);
    }
};

void finish(ArrowArray* a) {
    ArrowError err;
    REQUIRE(ArrowArrayFinishBuildingDefault(a, &err) == NANOARROW_OK);
}

Series make_timestamp(const std::vector<std::int64_t>& values, TimeUnit unit,
                      const char* tz) {
    ArrowTimeUnit au = unit == TimeUnit::Second  ? NANOARROW_TIME_UNIT_SECOND
                       : unit == TimeUnit::Milli ? NANOARROW_TIME_UNIT_MILLI
                       : unit == TimeUnit::Micro ? NANOARROW_TIME_UNIT_MICRO
                                                 : NANOARROW_TIME_UNIT_NANO;
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_TIMESTAMP, au,
                                       tz) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_decimal128(std::int32_t precision, std::int32_t scale,
                       const std::vector<std::int64_t>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDecimal(&b.schema, NANOARROW_TYPE_DECIMAL128,
                                      precision, scale) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values) {
        ArrowDecimal dec;
        ArrowDecimalInit(&dec, 128, precision, scale);
        ArrowDecimalSetInt(&dec, v);
        REQUIRE(ArrowArrayAppendDecimal(&b.array, &dec) == NANOARROW_OK);
    }
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW

DataFrame basic_frame() {
    DataFrame df;
    df.names = {"a", "b", "cat"};
    df.columns.push_back(i64({1, 2, 3, 4, 5}));
    df.columns.push_back(f64({1.5, 2.5, 3.5, 4.5, 5.5}));
    df.columns.push_back(Series::strings({"x", "y", "x", "z", "y"}));
    return df;
}

// Two groups of three rows, wide enough to exercise every AggOp: a numeric
// value column, a by-column linearly related to it (co-moment/argmax/list
// ops), a raw-repr string column, and a ts/dur pair for occupancy.
DataFrame mixed_frame() {
    DataFrame df;
    df.names = {"cat", "a", "b", "s", "ts", "dur"};
    df.columns.push_back(Series::strings({"x", "x", "x", "y", "y", "y"}));
    df.columns.push_back(i64({1, 2, 3, 10, 20, 30}));
    df.columns.push_back(f64({1.5, 2.5, 3.5, 10.5, 20.5, 30.5}));
    df.columns.push_back(Series::strings({"p", "q", "r", "m", "n", "o"}));
    df.columns.push_back(i64({100, 150, 200, 1000, 1050, 1100}));
    df.columns.push_back(i64({10, 20, 15, 5, 25, 10}));
    return df;
}

}  // namespace

TEST_SUITE("LazyFrame output_schema") {
    TEST_CASE("select subsets in order, types preserved") {
        LazyFrame lf = LazyFrame::scan(src(basic_frame())).select({"cat", "a"});
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 2);
        CHECK(s.fields[0] == Field{"cat", scalar(TypeId::String), true});
        CHECK(s.fields[1] == Field{"a", scalar(TypeId::Int64), true});
        check_matches_collect(lf);
    }

    TEST_CASE("rename keeps types, renames positionally") {
        LazyFrame lf =
            LazyFrame::scan(src(basic_frame())).rename({"x", "y", "z"});
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 3);
        CHECK(s.fields[0].name == "x");
        CHECK(s.fields[0].type == scalar(TypeId::Int64));
        check_matches_collect(lf);
    }

    TEST_CASE(
        "filter/sort_by/head/tail/slice/reverse/unique/drop_nulls keep types") {
        LazyFrame base = LazyFrame::scan(src(basic_frame()));
        check_matches_collect(base.filter(col(0) > std::int64_t{1}));
        check_matches_collect(base.sort_by("a"));
        check_matches_collect(base.head(2));
        check_matches_collect(base.tail(2));
        check_matches_collect(base.slice(1, 2));
        check_matches_collect(base.reverse());
        check_matches_collect(base.unique());
        check_matches_collect(base.drop_duplicates());
        check_matches_collect(base.drop_nulls());
    }

    TEST_CASE("with_column types the new column from its Expr") {
        LazyFrame lf = LazyFrame::scan(src(basic_frame()))
                           .with_column("d", col(0) + col(1));
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 4);
        CHECK(s.fields[3] == Field{"d", scalar(TypeId::Float64), true});
        check_matches_collect(lf);
    }

    TEST_CASE("with_column replacing an existing name keeps its position") {
        LazyFrame lf = LazyFrame::scan(src(basic_frame()))
                           .with_column("a", col(0) > std::int64_t{1});
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 3);
        CHECK(s.fields[0] == Field{"a", scalar(TypeId::Bool), true});
        check_matches_collect(lf);
    }

    TEST_CASE("with_row_index prepends an Int64 field") {
        LazyFrame lf =
            LazyFrame::scan(src(basic_frame())).with_row_index("idx");
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 4);
        CHECK(s.fields[0] == Field{"idx", scalar(TypeId::Int64), true});
        check_matches_collect(lf);
    }

    TEST_CASE("explode reports the List element type") {
        DataFrame df;
        df.names = {"k", "vals"};
        df.columns.push_back(i64({1, 2}));
        std::vector<std::int32_t> off{0, 2, 3};
        df.columns.push_back(Series::list(off, i64({10, 20, 30})));
        LazyFrame lf = LazyFrame::scan(src(std::move(df))).explode("vals");
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 2);
        CHECK(s.fields[1] == Field{"vals", scalar(TypeId::Int64), true});
        check_matches_collect(lf);
    }

    TEST_CASE("fill_null keeps the column's type") {
        LazyFrame lf =
            LazyFrame::scan(src(basic_frame())).fill_null(std::int64_t{0});
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 3);
        CHECK(s.fields[0].type == scalar(TypeId::Int64));
        CHECK(s.fields[1].type == scalar(TypeId::Float64));
        check_matches_collect(lf);
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    TEST_CASE(
        "fill_null leaves a Timestamp column (and its timezone) untouched") {
        // dftu_series_fillna only runs on a numeric-dispatchable column, so a
        // Timestamp column is passed through unchanged, tz included.
        DataFrame df;
        df.names = {"ts"};
        df.columns.push_back(
            make_timestamp({1, 2, 3}, TimeUnit::Micro, "America/Chicago"));
        LazyFrame lf =
            LazyFrame::scan(src(std::move(df))).fill_null(std::int64_t{0});
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 1);
        CHECK(s.fields[0].type ==
              timestamp(TimeUnit::Micro, "America/Chicago"));
        check_matches_collect(lf);
    }

    TEST_CASE(
        "select/rename/filter/sort_by preserve a Timestamp's tz and a "
        "Decimal128's precision/scale") {
        DataFrame df;
        df.names = {"ts", "dec", "n"};
        df.columns.push_back(make_timestamp({1, 2, 3}, TimeUnit::Nano, "UTC"));
        df.columns.push_back(make_decimal128(38, 9, {1, 2, 3}));
        df.columns.push_back(i64({3, 1, 2}));

        LazyFrame lf = LazyFrame::scan(src(std::move(df)))
                           .select({"ts", "dec", "n"})
                           .rename({"ts2", "dec2", "n2"})
                           .filter(col(2) > std::int64_t{0})
                           .sort_by("n2");
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 3);
        CHECK(s.fields[0] ==
              Field{"ts2", timestamp(TimeUnit::Nano, "UTC"), true});
        CHECK(s.fields[1] == Field{"dec2", decimal128(38, 9), true});
        check_matches_collect(lf);
    }
#endif  // DFTRACER_UTILS_ENABLE_ARROW

    TEST_CASE("group_by types Count/Sum/Mean/Min/Max from the value column") {
        LazyFrame lf = LazyFrame::scan(src(basic_frame()))
                           .group_by("cat", {GroupAgg{Agg::Count, "", "n"},
                                             GroupAgg{Agg::Sum, "a", "sum_a"},
                                             GroupAgg{Agg::Mean, "a", "mean_a"},
                                             GroupAgg{Agg::Min, "b", "min_b"},
                                             GroupAgg{Agg::Max, "b", "max_b"}});
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 6);
        CHECK(s.fields[0] == Field{"cat", scalar(TypeId::String), true});
        CHECK(s.fields[1] == Field{"n", scalar(TypeId::Int64), true});
        CHECK(s.fields[2] == Field{"sum_a", scalar(TypeId::Int64), true});
        CHECK(s.fields[3] == Field{"mean_a", scalar(TypeId::Float64), true});
        CHECK(s.fields[4] == Field{"min_b", scalar(TypeId::Float64), true});
        CHECK(s.fields[5] == Field{"max_b", scalar(TypeId::Float64), true});
        check_matches_collect(lf);
    }

    TEST_CASE("data-dependent ops report an empty schema, not a guess") {
        LazyFrame base = LazyFrame::scan(src(basic_frame()));
        CHECK(base.pivot("cat", "cat", "a").output_schema().fields.empty());
        CHECK(base.to_dummies("cat").output_schema().fields.empty());
        CHECK(base.describe().output_schema().fields.empty());
    }

    TEST_CASE("output_schema over a chain still matches collect()") {
        LazyFrame lf = LazyFrame::scan(src(basic_frame()))
                           .filter(col(0) > std::int64_t{0})
                           .with_column("d", col(0) * col(1))
                           .sort_by("d", true)
                           .head(3);
        check_matches_collect(lf);
    }

    TEST_CASE(
        "group_by types Var/Std/Skew/Kurt/SumSq/CountValid/Pct/Hist as "
        "Float64/Int64/nested") {
        LazyFrame lf =
            LazyFrame::scan(src(mixed_frame()))
                .group_by("cat", {GroupAgg{Agg::Var, "a", "var_a"},
                                  GroupAgg{Agg::Std, "a", "std_a"},
                                  GroupAgg{Agg::Skew, "a", "skew_a"},
                                  GroupAgg{Agg::Kurt, "a", "kurt_a"},
                                  GroupAgg{Agg::SumSq, "a", "sumsq_a"},
                                  GroupAgg{Agg::CountValid, "a", "cv_a"},
                                  GroupAgg{Agg::Pct, "a", "p50_a", 0.5},
                                  GroupAgg{Agg::Hist, "a", "hist_a"}});
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 9);
        CHECK(s.fields[1] == Field{"var_a", scalar(TypeId::Float64), true});
        CHECK(s.fields[2] == Field{"std_a", scalar(TypeId::Float64), true});
        CHECK(s.fields[3] == Field{"skew_a", scalar(TypeId::Float64), true});
        CHECK(s.fields[4] == Field{"kurt_a", scalar(TypeId::Float64), true});
        CHECK(s.fields[5] == Field{"sumsq_a", scalar(TypeId::Float64), true});
        CHECK(s.fields[6] == Field{"cv_a", scalar(TypeId::Int64), true});
        CHECK(s.fields[7] == Field{"p50_a", scalar(TypeId::Float64), true});
        CHECK(s.fields[8] ==
              Field{"hist_a",
                    list_of(struct_of(
                        {Field{"lo", scalar(TypeId::Float64), true},
                         Field{"hi", scalar(TypeId::Float64), true},
                         Field{"count", scalar(TypeId::Uint64), true}})),
                    true});
        check_matches_collect(lf);
    }

    TEST_CASE(
        "group_by types First/Last from the value column, Int64 widened and "
        "String passed through") {
        LazyFrame lf =
            LazyFrame::scan(src(mixed_frame()))
                .group_by("cat", {GroupAgg{Agg::First, "a", "first_a"},
                                  GroupAgg{Agg::Last, "a", "last_a"},
                                  GroupAgg{Agg::First, "s", "first_s"},
                                  GroupAgg{Agg::Last, "s", "last_s"}});
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 5);
        CHECK(s.fields[1] == Field{"first_a", scalar(TypeId::Int64), true});
        CHECK(s.fields[2] == Field{"last_a", scalar(TypeId::Int64), true});
        CHECK(s.fields[3] == Field{"first_s", scalar(TypeId::String), true});
        CHECK(s.fields[4] == Field{"last_s", scalar(TypeId::String), true});
        check_matches_collect(lf);
    }

    TEST_CASE("group_by types ArgMax/ArgMin/BitOr/Distinct/SetUnion") {
        LazyFrame lf =
            LazyFrame::scan(src(mixed_frame()))
                .group_by("cat",
                          {GroupAgg{Agg::ArgMax, "s", "argmax_s", 0, "b"},
                           GroupAgg{Agg::ArgMin, "s", "argmin_s", 0, "b"},
                           GroupAgg{Agg::BitOr, "a", "bitor_a"},
                           GroupAgg{Agg::Distinct, "s", "distinct_s", 4},
                           GroupAgg{Agg::SetUnion, "s", "setunion_s"}});
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 6);
        CHECK(s.fields[1] == Field{"argmax_s", scalar(TypeId::String), true});
        CHECK(s.fields[2] == Field{"argmin_s", scalar(TypeId::String), true});
        CHECK(s.fields[3] == Field{"bitor_a", scalar(TypeId::Uint64), true});
        CHECK(s.fields[4] == Field{"distinct_s", scalar(TypeId::Int64), true});
        CHECK(s.fields[5] == Field{"setunion_s", scalar(TypeId::String), true});
        check_matches_collect(lf);
    }

    TEST_CASE(
        "group_by types the list-valued aggregates ListSorted/TopK/BottomK/"
        "ApproxTopK/Sample") {
        LazyFrame lf =
            LazyFrame::scan(src(mixed_frame()))
                .group_by("cat",
                          {GroupAgg{Agg::ListSorted, "s", "list_s", 0, "b"},
                           GroupAgg{Agg::TopK, "s", "topk_s", 2, "b"},
                           GroupAgg{Agg::BottomK, "s", "bottomk_s", 2, "b"},
                           GroupAgg{Agg::ApproxTopK, "s", "approxtopk_s", 3},
                           GroupAgg{Agg::Sample, "s", "sample_s", 2}});
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 6);
        CHECK(s.fields[1] ==
              Field{"list_s", list_of(scalar(TypeId::String)), true});
        CHECK(s.fields[2] ==
              Field{"topk_s", list_of(scalar(TypeId::String)), true});
        CHECK(s.fields[3] ==
              Field{"bottomk_s", list_of(scalar(TypeId::String)), true});
        CHECK(s.fields[4] ==
              Field{"approxtopk_s",
                    list_of(struct_of(
                        {Field{"value", scalar(TypeId::String), true},
                         Field{"count", scalar(TypeId::Uint64), true}})),
                    true});
        CHECK(s.fields[5] ==
              Field{"sample_s", list_of(scalar(TypeId::String)), true});
        check_matches_collect(lf);
    }

    TEST_CASE("group_by types the co-moment aggregates as Float64") {
        LazyFrame lf =
            LazyFrame::scan(src(mixed_frame()))
                .group_by(
                    "cat",
                    {GroupAgg{Agg::Corr, "b", "corr_ab", 0, "a"},
                     GroupAgg{Agg::CovarPop, "b", "covpop_ab", 0, "a"},
                     GroupAgg{Agg::CovarSamp, "b", "covsamp_ab", 0, "a"},
                     GroupAgg{Agg::RegrSlope, "b", "slope_ab", 0, "a"},
                     GroupAgg{Agg::RegrIntercept, "b", "intercept_ab", 0, "a"},
                     GroupAgg{Agg::RegrR2, "b", "r2_ab", 0, "a"}});
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 7);
        for (std::size_t i = 1; i < s.fields.size(); ++i)
            CHECK(s.fields[i].type == scalar(TypeId::Float64));
        check_matches_collect(lf);
    }

    TEST_CASE("group_by types the occupancy aggregates as Float64") {
        LazyFrame lf =
            LazyFrame::scan(src(mixed_frame()))
                .group_by("cat",
                          {GroupAgg{Agg::Busy, "ts", "busy", 0, "dur"},
                           GroupAgg{Agg::Concurrency, "ts", "conc", 0, "dur"},
                           GroupAgg{Agg::Utilization, "ts", "util", 0, "dur"},
                           GroupAgg{Agg::Active, "ts", "active", 0, "dur"}});
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 5);
        for (std::size_t i = 1; i < s.fields.size(); ++i)
            CHECK(s.fields[i].type == scalar(TypeId::Float64));
        check_matches_collect(lf);
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    TEST_CASE(
        "group_by widens a Decimal128 or Timestamp value column through the "
        "Int64 domain") {
        DataFrame df;
        df.names = {"cat", "dec", "ts"};
        df.columns.push_back(Series::strings({"x", "x", "y"}));
        df.columns.push_back(make_decimal128(38, 9, {100, 200, 300}));
        df.columns.push_back(
            make_timestamp({1, 2, 3}, TimeUnit::Micro, "America/Chicago"));
        LazyFrame lf =
            LazyFrame::scan(src(std::move(df)))
                .group_by("cat", {GroupAgg{Agg::Sum, "dec", "sum_dec"},
                                  GroupAgg{Agg::First, "dec", "first_dec"},
                                  GroupAgg{Agg::Sum, "ts", "sum_ts"},
                                  GroupAgg{Agg::First, "ts", "first_ts"}});
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 5);
        // Decimal128 and Timestamp both fall through col_domain's default
        // branch to I64, so Sum/First widen to plain Int64: the decimal's
        // scale and the timestamp's unit/timezone are both lost. Documented
        // in agg_output_type's contract; agg_finalize does the same, so this
        // is a real limitation, not a schema/collect() disagreement.
        CHECK(s.fields[1] == Field{"sum_dec", scalar(TypeId::Int64), true});
        CHECK(s.fields[2] == Field{"first_dec", scalar(TypeId::Int64), true});
        CHECK(s.fields[3] == Field{"sum_ts", scalar(TypeId::Int64), true});
        CHECK(s.fields[4] == Field{"first_ts", scalar(TypeId::Int64), true});
        check_matches_collect(lf);
    }
#endif  // DFTRACER_UTILS_ENABLE_ARROW
}
