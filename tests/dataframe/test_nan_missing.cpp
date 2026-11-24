// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dataframe/expr_ok.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>
#include <dftracer/utils/dataframe/kernels/nan_rows.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

const double NAN_V = std::numeric_limits<double>::quiet_NaN();

bool bit(const Series& s, std::int64_t i) {
    return (static_cast<const std::uint8_t*>(s.data<void>())[i >> 3] >>
            (i & 7)) &
           1;
}

dftu_scalar f64_scalar(double v) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_F64;
    s.value.d = v;
    return s;
}

dftu_scalar null_scalar() {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_NULL;
    return s;
}

dftu_scalar str_scalar(dstd::CStringView t) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_STR;
    s.len = static_cast<std::uint32_t>(t.size());
    s.value.s = t.c_str();
    return s;
}

// 1.5, null, NaN, 4.0 : row 1 is null, row 2 is NaN.
struct Floats {
    double values[4] = {1.5, 0.0, NAN_V, 4.0};
    std::uint8_t valid = 0b1101;
    Series series() const { return Series::flat_f64(values, 4, &valid); }
};

DataFrame run(coro::CoroTask<dstd::Result<DataFrame>> t) {
    DataFrame out = dftu_utils_test::ok(
        dftracer::utils::default_runtime().submit(std::move(t)).get());
    for (Series& c : out.columns) c = c.materialize();
    return out;
}

// A frame of a float column (1.5, null, NaN, 4.0), an int column and a string
// column.
DataFrame mixed_frame(const Floats& f, dstd::Span<const std::int64_t> ints,
                      dstd::Span<const dstd::StringView> strs) {
    DataFrame df;
    df.names = dstd::make_shared_strings({"f", "i", "s"});
    df.columns.push_back(f.series());
    df.columns.push_back(Series::flat_i64(ints.data(), 4));
    df.columns.push_back(Series::strings(
        dstd::Span<const dstd::StringView>(strs.data(), strs.size()), nullptr));
    return df;
}

}  // namespace

TEST_CASE("na_mask and notna_mask: null or NaN on a float column") {
    const Floats f;
    const Series s = f.series();
    const Series na = s.na_mask();
    const Series ok = s.notna_mask();
    REQUIRE(na.valid());
    REQUIRE(ok.valid());
    CHECK(na.null_count() == 0);
    CHECK(ok.null_count() == 0);
    const bool want_na[4] = {false, true, true, false};
    for (std::int64_t i = 0; i < 4; ++i) {
        CHECK(bit(na, i) == want_na[i]);
        CHECK(bit(ok, i) == !want_na[i]);
    }
    // is_nan alone leaves the null row null and the NaN row true.
    const Series nan = s.is_nan();
    CHECK(nan.is_null(1));
    CHECK(bit(nan, 2));
}

TEST_CASE("na_mask on a float column of NaN only, nulls only and neither") {
    const double nans[3] = {NAN_V, 1.0, NAN_V};
    const Series a = Series::flat_f64(nans, 3);
    CHECK(bit(a.na_mask(), 0));
    CHECK_FALSE(bit(a.na_mask(), 1));
    CHECK(bit(a.na_mask(), 2));

    const double vals[3] = {1.0, 2.0, 3.0};
    const std::uint8_t none_valid = 0;
    const Series b = Series::flat_f64(vals, 3, &none_valid);
    for (std::int64_t i = 0; i < 3; ++i) CHECK(bit(b.na_mask(), i));
    for (std::int64_t i = 0; i < 3; ++i) CHECK_FALSE(bit(b.notna_mask(), i));

    const Series c = Series::flat_f64(vals, 3);
    for (std::int64_t i = 0; i < 3; ++i) CHECK_FALSE(bit(c.na_mask(), i));
    for (std::int64_t i = 0; i < 3; ++i) CHECK(bit(c.notna_mask(), i));
}

TEST_CASE("na_mask on other types is the null mask") {
    const std::int64_t vals[3] = {1, 2, 3};
    const std::uint8_t valid = 0b101;
    const Series ints = Series::flat_i64(vals, 3, &valid);
    CHECK_FALSE(bit(ints.na_mask(), 0));
    CHECK(bit(ints.na_mask(), 1));
    CHECK(bit(ints.notna_mask(), 2));

    const dstd::Vector<dstd::StringView> rows = {"a", "b", "c"};
    const Series strs = Series::strings(
        dstd::Span<const dstd::StringView>(rows.data(), rows.size()), &valid);
    CHECK_FALSE(bit(strs.na_mask(), 0));
    CHECK(bit(strs.na_mask(), 1));
    CHECK_FALSE(bit(strs.notna_mask(), 1));
}

TEST_CASE("na_mask of an empty column is empty") {
    const Series e = Series::flat_f64(nullptr, 0);
    CHECK(e.na_mask().length() == 0);
    CHECK(e.notna_mask().length() == 0);
}

TEST_CASE("fill_nan fills the NaN and keeps the nulls") {
    const Floats f;
    const Series r = f.series().fill_nan(9.0);
    REQUIRE(r.valid());
    CHECK(r.length() == 4);
    CHECK(r.data<double>()[0] == 1.5);
    CHECK(r.is_null(1));
    CHECK(r.data<double>()[2] == 9.0);
    CHECK(r.data<double>()[3] == 4.0);
    CHECK(r.null_count() == 1);
}

TEST_CASE("fill_nan with an integer fills a float column") {
    const Floats f;
    const Series r = f.series().fill_nan(std::int64_t{7});
    REQUIRE(r.valid());
    CHECK(r.data<double>()[2] == 7.0);
}

TEST_CASE("fill_nan with a null scalar turns the NaN into a null") {
    const Floats f;
    const Series r = f.series().fill_nan(Scalar{null_scalar()});
    REQUIRE(r.valid());
    CHECK(r.null_count() == 2);
    CHECK_FALSE(r.is_null(0));
    CHECK(r.is_null(1));
    CHECK(r.is_null(2));
    CHECK_FALSE(r.is_null(3));
    CHECK(r.data<double>()[0] == 1.5);
    CHECK(r.data<double>()[3] == 4.0);
}

TEST_CASE("fill_nan on a Float32 column") {
    const float vals[3] = {1.0f, std::numeric_limits<float>::quiet_NaN(), 3.0f};
    const Series s = Series::flat(TypeId::Float32, vals, 3);
    const Series r = s.fill_nan(2.5);
    REQUIRE(r.valid());
    CHECK(r.data<float>()[1] == 2.5f);
    CHECK(r.data<float>()[2] == 3.0f);
}

TEST_CASE("fill_nan leaves a column of another type as it is") {
    const std::int64_t vals[3] = {1, 2, 3};
    const std::uint8_t valid = 0b101;
    const Series ints = Series::flat_i64(vals, 3, &valid);
    const Series r = ints.fill_nan(0.0);
    REQUIRE(r.valid());
    CHECK(r.length() == 3);
    CHECK(r.null_count() == 1);
    CHECK(r.is_null(1));
    CHECK(r.data<std::int64_t>()[2] == 3);

    const dstd::Vector<dstd::StringView> rows = {"a", "b"};
    const Series strs = Series::strings(
        dstd::Span<const dstd::StringView>(rows.data(), rows.size()), nullptr);
    const Series same = strs.fill_nan(0.0);
    REQUIRE(same.valid());
    CHECK(same.string_at(1) == "b");
}

TEST_CASE("fill_nan refuses a string fill on a float column") {
    const Floats f;
    CHECK_FALSE(f.series().fill_nan(Scalar{str_scalar("x")}).valid());
}

TEST_CASE("fill_nan of an empty float column is empty") {
    const Series e = Series::flat_f64(nullptr, 0);
    const Series r = e.fill_nan(1.0);
    REQUIRE(r.valid());
    CHECK(r.length() == 0);
}

TEST_CASE("drop_nans drops the NaN rows and keeps the nulls") {
    const Floats f;
    const Series r = f.series().drop_nans().materialize();
    REQUIRE(r.valid());
    CHECK(r.length() == 3);
    CHECK(r.null_count() == 1);
    CHECK(r.is_null(1));
    CHECK(r.data<double>()[0] == 1.5);
    CHECK(r.data<double>()[2] == 4.0);
}

TEST_CASE("drop_nans on other types and on an empty column keeps every row") {
    const std::int64_t vals[3] = {1, 2, 3};
    const std::uint8_t valid = 0b101;
    const Series ints = Series::flat_i64(vals, 3, &valid);
    const Series r = ints.drop_nans().materialize();
    REQUIRE(r.valid());
    CHECK(r.length() == 3);
    CHECK(r.null_count() == 1);

    const Series e = Series::flat_f64(nullptr, 0);
    CHECK(e.drop_nans().length() == 0);
}

TEST_CASE("the NaN ops through the C ABI are null safe") {
    CHECK(dftu_series_na_mask(nullptr) == nullptr);
    CHECK(dftu_series_notna_mask(nullptr) == nullptr);
    CHECK(dftu_series_fill_nan(nullptr, f64_scalar(0.0)) == nullptr);
    CHECK(dftu_series_drop_nans(nullptr) == nullptr);
    CHECK(dftu_dataframe_drop_nans(nullptr) == nullptr);
    CHECK(dftu_dataframe_fill_nan(nullptr, f64_scalar(0.0)) == nullptr);
    CHECK(dftu_lazyframe_drop_nans(nullptr) == nullptr);
    CHECK(dftu_lazyframe_fill_nan(nullptr, f64_scalar(0.0)) == nullptr);
}

TEST_CASE("DataFrame fill_nan fills the float columns only") {
    const Floats f;
    const dstd::Vector<std::int64_t> ints{1, 2, 3, 4};
    const dstd::Vector<dstd::StringView> strs{"a", "b", "c", "d"};
    DataFrame df = mixed_frame(f, ints, strs);
    const DataFrame r = dftu_utils_test::ok(df.fill_nan(0.0));
    REQUIRE(r.columns.size() == 3);
    CHECK(r.columns[0].data<double>()[2] == 0.0);
    CHECK(r.columns[0].is_null(1));
    CHECK(r.columns[1].data<std::int64_t>()[2] == 3);
    CHECK(r.columns[2].string_at(2) == "c");

    const DataFrame n = dftu_utils_test::ok(df.fill_nan(Scalar{null_scalar()}));
    CHECK(n.columns[0].is_null(1));
    CHECK(n.columns[0].is_null(2));
    CHECK(n.columns[1].null_count() == 0);
}

TEST_CASE("DataFrame drop_nans drops a row with a NaN in a float column") {
    const Floats f;
    const dstd::Vector<std::int64_t> ints{1, 2, 3, 4};
    const dstd::Vector<dstd::StringView> strs{"a", "b", "c", "d"};
    DataFrame df = mixed_frame(f, ints, strs);
    DataFrame r = dftu_utils_test::ok(df.drop_nans());
    for (Series& c : r.columns) c = c.materialize();
    REQUIRE(r.num_rows() == 3);
    CHECK(r.columns[1].data<std::int64_t>()[0] == 1);
    CHECK(r.columns[1].data<std::int64_t>()[1] == 2);
    CHECK(r.columns[1].data<std::int64_t>()[2] == 4);
    CHECK(r.columns[0].is_null(1));
}

TEST_CASE("DataFrame drop_nans with no float column keeps every row") {
    DataFrame df;
    const dstd::Vector<std::int64_t> ints{1, 2, 3};
    df.names = dstd::make_shared_strings({"i"});
    df.columns.push_back(Series::flat_i64(ints.data(), 3));
    CHECK(dftu_utils_test::ok(df.drop_nans()).num_rows() == 3);
    CHECK(dftu_utils_test::ok(DataFrame{}.drop_nans()).num_rows() == 0);
}

TEST_CASE("LazyFrame fill_nan and drop_nans match the eager frame") {
    const Floats f;
    const dstd::Vector<std::int64_t> ints{1, 2, 3, 4};
    const dstd::Vector<dstd::StringView> strs{"a", "b", "c", "d"};
    DataFrame df = mixed_frame(f, ints, strs);

    const DataFrame filled = run(df.lazy().fill_nan(0.0).collect());
    REQUIRE(filled.num_rows() == 4);
    CHECK(filled.column("f").data<double>()[2] == 0.0);
    CHECK(filled.column("f").is_null(1));

    const DataFrame nulled =
        run(df.lazy().fill_nan(Scalar{null_scalar()}).collect());
    CHECK(nulled.column("f").is_null(2));

    const DataFrame dropped = run(df.lazy().drop_nans().collect(2));
    REQUIRE(dropped.num_rows() == 3);
    CHECK(dropped.column("i").data<std::int64_t>()[2] == 4);
    CHECK(dropped.column("f").is_null(1));
}

TEST_CASE("lazy fill_nan and drop_nans register as frame ops") {
    CHECK(dftu_op_find("dftu.frame.drop_nans") != nullptr);
    CHECK(dftu_op_find("dftu.frame.fill_nan") != nullptr);
    CHECK(dftu_op_find("dftu.lazy.drop_nans") != nullptr);
    CHECK(dftu_op_find("dftu.lazy.fill_nan") != nullptr);
    CHECK(dftu_op_find("dftu.series.na_mask") != nullptr);
    CHECK(dftu_op_find("dftu.series.notna_mask") != nullptr);
    CHECK(dftu_op_find("dftu.series.fill_nan") != nullptr);
    CHECK(dftu_op_find("dftu.series.drop_nans") != nullptr);
}

TEST_CASE("na_mask and notna_mask on a Float32 column") {
    const float vals[4] = {1.0f, std::numeric_limits<float>::quiet_NaN(), 3.0f,
                           0.0f};
    const std::uint8_t valid = 0b0111;
    const Series s = Series::flat(TypeId::Float32, vals, 4, &valid);
    const bool want_na[4] = {false, true, false, true};
    for (std::int64_t i = 0; i < 4; ++i) {
        CHECK(bit(s.na_mask(), i) == want_na[i]);
        CHECK(bit(s.notna_mask(), i) == !want_na[i]);
    }
}

TEST_CASE("drop_nans on a Float32 column") {
    const float vals[4] = {1.0f, std::numeric_limits<float>::quiet_NaN(), 3.0f,
                           4.0f};
    const Series s = Series::flat(TypeId::Float32, vals, 4);
    const Series r = s.drop_nans().materialize();
    REQUIRE(r.valid());
    CHECK(r.length() == 3);
    CHECK(r.data<float>()[1] == 3.0f);
}

TEST_CASE("fill_nan and drop_nans share a column with no NaN") {
    const double vals[3] = {1.0, 2.0, 3.0};
    const std::uint8_t valid = 0b101;
    const Series s = Series::flat_f64(vals, 3, &valid);
    CHECK(s.fill_nan(9.0).data<double>() == s.data<double>());
    CHECK(s.fill_nan(Scalar{null_scalar()}).data<double>() == s.data<double>());
    CHECK(s.drop_nans().data<double>() == s.data<double>());
}

TEST_CASE("a frame with no NaN keeps its column buffers") {
    const double vals[3] = {1.0, 2.0, 3.0};
    const dstd::Vector<std::int64_t> ints{1, 2, 3};
    DataFrame df;
    df.names = dstd::make_shared_strings({"f", "i"});
    df.columns.push_back(Series::flat_f64(vals, 3));
    df.columns.push_back(Series::flat_i64(ints.data(), 3));
    const DataFrame filled = dftu_utils_test::ok(df.fill_nan(0.0));
    const DataFrame dropped = dftu_utils_test::ok(df.drop_nans());
    for (std::size_t c = 0; c < 2; ++c) {
        CHECK(filled.columns[c].data<void>() == df.columns[c].data<void>());
        CHECK(dropped.columns[c].data<void>() == df.columns[c].data<void>());
    }
}

TEST_CASE("expr_is_na is row-wise and sees a NaN as missing") {
    const Floats f;
    const Series s = f.series();
    const Series* in[1] = {&s};
    const Series na = dftu_utils_test::eval_ok(
        expr_is_na(expr_col(0), true), dstd::Span<const Series* const>(in, 1));
    const Series ok = dftu_utils_test::eval_ok(
        expr_is_na(expr_col(0), false), dstd::Span<const Series* const>(in, 1));
    const bool want_na[4] = {false, true, true, false};
    for (std::int64_t i = 0; i < 4; ++i) {
        CHECK(bit(na, i) == want_na[i]);
        CHECK(bit(ok, i) == !want_na[i]);
    }
    // The null-only node still leaves the NaN row present.
    const Series nulls =
        dftu_utils_test::eval_ok(expr_is_null(expr_col(0), true),
                                 dstd::Span<const Series* const>(in, 1));
    CHECK_FALSE(bit(nulls, 2));
}

TEST_CASE("expr_is_na on a Float32 and an integer column") {
    const float vals[3] = {1.0f, std::numeric_limits<float>::quiet_NaN(), 3.0f};
    const Series s = Series::flat(TypeId::Float32, vals, 3);
    const Series* in[1] = {&s};
    const Series na = dftu_utils_test::eval_ok(
        expr_is_na(expr_col(0), true), dstd::Span<const Series* const>(in, 1));
    CHECK_FALSE(bit(na, 0));
    CHECK(bit(na, 1));
    const std::int64_t ints[2] = {1, 2};
    const std::uint8_t valid = 0b01;
    const Series i = Series::flat_i64(ints, 2, &valid);
    const Series* in2[1] = {&i};
    const Series na2 = dftu_utils_test::eval_ok(
        expr_is_na(expr_col(0), true), dstd::Span<const Series* const>(in2, 1));
    CHECK_FALSE(bit(na2, 0));
    CHECK(bit(na2, 1));
}

TEST_CASE(
    "expr_fillna with nan fills a NaN and a null, and keeps other types") {
    const Floats f;
    const Series s = f.series();
    const Series* in[1] = {&s};
    const Series r = dftu_utils_test::eval_ok(
        expr_fillna(expr_col(0), Scalar{f64_scalar(7.0)}, true),
        dstd::Span<const Series* const>(in, 1));
    CHECK(r.null_count() == 0);
    CHECK(r.data<double>()[1] == 7.0);
    CHECK(r.data<double>()[2] == 7.0);
    const Series plain = dftu_utils_test::eval_ok(
        expr_fillna(expr_col(0), Scalar{f64_scalar(7.0)}),
        dstd::Span<const Series* const>(in, 1));
    CHECK(std::isnan(plain.data<double>()[2]));

    const std::int64_t ints[2] = {1, 2};
    const std::uint8_t valid = 0b01;
    const Series i = Series::flat_i64(ints, 2, &valid);
    const Series* in2[1] = {&i};
    const Series ri = dftu_utils_test::eval_ok(
        expr_fillna(expr_col(0), Scalar{f64_scalar(7.0)}, true),
        dstd::Span<const Series* const>(in2, 1));
    CHECK(ri.type() == TypeId::Int64);
}

TEST_CASE("a lazy filter on expr_is_na streams and keeps the missing rows") {
    const Floats f;
    const dstd::Vector<std::int64_t> ints{1, 2, 3, 4};
    const dstd::Vector<dstd::StringView> strs{"a", "b", "c", "d"};
    DataFrame df = mixed_frame(f, ints, strs);
    const DataFrame out =
        run(df.lazy().filter(expr_is_na(expr_col(0), true)).collect(2));
    REQUIRE(out.num_rows() == 2);
    CHECK(out.column("i").data<std::int64_t>()[0] == 2);
    CHECK(out.column("i").data<std::int64_t>()[1] == 3);
}

TEST_CASE("a select on a Float32 column with an int scalar stays Float32") {
    const float vals[3] = {1.5f, std::numeric_limits<float>::quiet_NaN(), 3.0f};
    const Series s = Series::flat(TypeId::Float32, vals, 3);
    const Series* in[1] = {&s};
    const Series r = dftu_utils_test::eval_ok(
        expr_select(expr_is_na(expr_col(0), true), expr_lit(std::int64_t{0}),
                    expr_col(0)),
        dstd::Span<const Series* const>(in, 1));
    REQUIRE(r.type() == TypeId::Float32);
    CHECK(r.data<float>()[0] == 1.5f);
    CHECK(r.data<float>()[1] == 0.0f);
    CHECK(r.data<float>()[2] == 3.0f);

    const double wide[3] = {1.0, 2.0, 3.0};
    const Series w = Series::flat_f64(wide, 3);
    const Series* in2[2] = {&s, &w};
    const Series r2 = dftu_utils_test::eval_ok(
        expr_select(expr_is_na(expr_col(0), true), expr_col(1), expr_col(0)),
        dstd::Span<const Series* const>(in2, 2));
    CHECK(r2.type() == TypeId::Float64);
    CHECK(r2.data<double>()[1] == 2.0);
}

TEST_CASE("expr_fillna with nan and a string fill on a float column fails") {
    const Floats f;
    const Series s = f.series();
    const Series* in[1] = {&s};
    const auto r = eval(expr_fillna(expr_col(0), Scalar{str_scalar("z")}, true),
                        dstd::Span<const Series* const>(in, 1));
    CHECK(!r.has_value());
}

TEST_CASE("expr_fillna with a string fill owns the text") {
    char buf[6] = "hello";
    const Expr e = expr_fillna(expr_col(0), Scalar{str_scalar(buf)});
    for (char& c : buf) {
        if (c != '\0') c = 'X';
    }
    const dstd::Vector<dstd::StringView> rows = {"a", "b", "c"};
    const std::uint8_t valid = 0b101;
    const Series s = Series::strings(
        dstd::Span<const dstd::StringView>(rows.data(), rows.size()), &valid);
    const Series* in[1] = {&s};
    const Series r =
        dftu_utils_test::eval_ok(e, dstd::Span<const Series* const>(in, 1));
    CHECK(r.null_count() == 0);
    CHECK(r.string_at(0) == "a");
    CHECK(r.string_at(1) == "hello");
    CHECK(r.string_at(2) == "c");
}

namespace {

// A Float64 column of nothing but non-NaN values and a NaN payload under a
// null slot.
struct NullSlotNan {
    double values[4] = {1.0, NAN_V, 3.0, 4.0};
    std::uint8_t valid = 0b1101;
    Series series() const { return Series::flat_f64(values, 4, &valid); }
};

Series chunked_f64(const double* a, std::int64_t na, const std::uint8_t* va,
                   const double* b, std::int64_t nb) {
    dstd::Vector<dstd::SharedPtr<dftu_series>> chunks;
    chunks.push_back(
        dstd::make_shared<dftu_series>(*Series::flat_f64(a, na, va).handle()));
    chunks.push_back(
        dstd::make_shared<dftu_series>(*Series::flat_f64(b, nb).handle()));
    return Series{make_chunked(std::move(chunks))};
}

}  // namespace

TEST_CASE("fill_nan and drop_nans share a column whose NaN sits under a null") {
    const NullSlotNan f;
    const Series s = f.series();
    CHECK(s.fill_nan(9.0).data<double>() == s.data<double>());
    CHECK(s.fill_nan(Scalar{null_scalar()}).data<double>() == s.data<double>());
    CHECK(s.drop_nans().data<double>() == s.data<double>());
}

TEST_CASE("fill_nan and drop_nans share a chunked column with no NaN") {
    const double a[3] = {1.0, NAN_V, 3.0};
    const std::uint8_t va = 0b101;
    const double b[2] = {4.0, 5.0};
    const Series s = chunked_f64(a, 3, &va, b, 2);
    REQUIRE(s.encoding() == Encoding::Chunked);
    for (const Series& r :
         {s.fill_nan(9.0), s.fill_nan(Scalar{null_scalar()}), s.drop_nans()}) {
        CHECK(r.encoding() == Encoding::Chunked);
        CHECK(r.handle()->data.data() == s.handle()->data.data());
    }
}

TEST_CASE("fill_nan and drop_nans still change a chunked column with a NaN") {
    const double a[2] = {1.0, 2.0};
    const double b[2] = {NAN_V, 5.0};
    const Series s = chunked_f64(a, 2, nullptr, b, 2);
    const Series filled = s.fill_nan(9.0).materialize();
    CHECK(filled.data<double>()[2] == 9.0);
    CHECK(s.drop_nans().materialize().length() == 3);
}

TEST_CASE("fill_nan and drop_nans share a selection with no NaN") {
    const NullSlotNan f;
    const Series s = f.series().drop_nulls();
    REQUIRE(s.encoding() == Encoding::Selection);
    CHECK(s.fill_nan(9.0).handle()->data.data() == s.handle()->data.data());
    CHECK(s.drop_nans().handle()->data.data() == s.handle()->data.data());

    const double vals[3] = {1.0, NAN_V, 3.0};
    const std::uint8_t valid = 0b110;
    const Series picked = Series::flat_f64(vals, 3, &valid).drop_nulls();
    REQUIRE(picked.encoding() == Encoding::Selection);
    CHECK(picked.drop_nans().materialize().length() == 1);
    CHECK(picked.fill_nan(9.0).materialize().data<double>()[0] == 9.0);
}

TEST_CASE("fillna refuses a string fill on a numeric column") {
    const std::int64_t vals[3] = {1, 2, 3};
    const std::uint8_t valid = 0b101;
    const Series ints = Series::flat_i64(vals, 3, &valid);
    CHECK_FALSE(ints.fillna(Scalar{str_scalar("z")}).valid());
    const Floats f;
    CHECK_FALSE(f.series().fillna(Scalar{str_scalar("z")}).valid());
    dftu_scalar err{};
    err.kind = DFTU_SCALAR_TAG_ERR;
    CHECK_FALSE(ints.fillna(Scalar{err}).valid());
}

TEST_CASE("fillna with a null scalar shares a numeric column") {
    const Floats f;
    const Series s = f.series();
    const Series r = s.fillna(Scalar{null_scalar()});
    REQUIRE(r.valid());
    CHECK(r.data<double>() == s.data<double>());
    CHECK(r.null_count() == 1);
}

TEST_CASE("DataFrame fill_null with a string fills only the string column") {
    const Floats f;
    const dstd::Vector<std::int64_t> ints{1, 2, 3, 4};
    const std::uint8_t ivalid = 0b1011;
    const dstd::Vector<dstd::StringView> strs{"a", "b", "c", "d"};
    const std::uint8_t svalid = 0b0101;
    DataFrame df;
    df.names = dstd::make_shared_strings({"f", "i", "s"});
    df.columns.push_back(f.series());
    df.columns.push_back(Series::flat_i64(ints.data(), 4, &ivalid));
    df.columns.push_back(Series::strings(
        dstd::Span<const dstd::StringView>(strs.data(), strs.size()), &svalid));
    const DataFrame r = df.fill_null(Scalar{str_scalar("z")});
    REQUIRE(r.columns.size() == 3);
    CHECK(r.columns[0].null_count() == 1);
    CHECK(r.columns[0].data<double>() == df.columns[0].data<double>());
    CHECK(r.columns[1].null_count() == 1);
    CHECK(r.columns[1].is_null(2));
    CHECK(r.columns[2].null_count() == 0);
    CHECK(r.columns[2].string_at(1) == "z");
    CHECK(r.columns[2].string_at(3) == "z");
}

TEST_CASE("expr_fillna refuses a fill of the wrong kind at compile time") {
    const std::int64_t vals[2] = {1, 2};
    const Series ints = Series::flat_i64(vals, 2);
    const Series* in[1] = {&ints};
    CHECK(!eval(expr_fillna(expr_col(0), Scalar{str_scalar("z")}),
                dstd::Span<const Series* const>(in, 1))
               .has_value());
    const DataType int_type = scalar(TypeId::Int64);
    CHECK(!infer_type(expr_fillna(expr_col(0), Scalar{str_scalar("z")}),
                      dstd::Span<const DataType>(&int_type, 1))
               .has_value());
    const DataType str_type = scalar(TypeId::String);
    CHECK(!infer_type(expr_fillna(expr_col(0), Scalar{f64_scalar(1.0)}),
                      dstd::Span<const DataType>(&str_type, 1))
               .has_value());

    const dstd::Vector<dstd::StringView> rows = {"a", "b"};
    const Series strs = Series::strings(
        dstd::Span<const dstd::StringView>(rows.data(), rows.size()));
    const Series* in2[1] = {&strs};
    CHECK(!eval(expr_fillna(expr_col(0), Scalar{f64_scalar(1.0)}),
                dstd::Span<const Series* const>(in2, 1))
               .has_value());
}

TEST_CASE(
    "DataFrame drop_nans keeps a chunked or selected column with no NaN") {
    const double a[3] = {1.0, NAN_V, 3.0};
    const std::uint8_t va = 0b101;
    const double b[2] = {4.0, 5.0};
    const NullSlotNan sel;
    DataFrame df;
    df.names = dstd::make_shared_strings({"c", "s"});
    df.columns.push_back(chunked_f64(a, 3, &va, b, 2));
    df.columns.push_back(
        Series::flat_f64(sel.values, 4, &sel.valid).drop_nulls());
    REQUIRE(df.columns[0].encoding() == Encoding::Chunked);
    REQUIRE(df.columns[1].encoding() == Encoding::Selection);
    for (const Series& c : df.columns) {
        std::int64_t nans = -1;
        CHECK_FALSE(keep_non_nan(c, &nans).valid());
        CHECK(nans == 0);
    }
    const DataFrame r = dftu_utils_test::ok(df.drop_nans());
    for (std::size_t c = 0; c < 2; ++c) {
        CHECK(r.columns[c].encoding() == df.columns[c].encoding());
        CHECK(r.columns[c].handle()->data.data() ==
              df.columns[c].handle()->data.data());
    }
}

TEST_CASE("fill_nan refuses a text fill on a frame, a plan and the C ABI") {
    const Floats f;
    const dstd::Vector<std::int64_t> ints{1, 2, 3, 4};
    const dstd::Vector<dstd::StringView> strs{"a", "b", "c", "d"};
    DataFrame df = mixed_frame(f, ints, strs);
    dftu_scalar err{};
    err.kind = DFTU_SCALAR_TAG_ERR;
    for (const dftu_scalar fill : {str_scalar("x"), err}) {
        dftu_utils_test::expect_error(df.fill_nan(Scalar{fill}),
                                      common::ErrorCode::InvalidArgument);
        const LazyFrame lazy = df.lazy().fill_nan(Scalar{fill});
        CHECK_FALSE(lazy.status().has_value());
        dftu_utils_test::expect_error(
            dftracer::utils::default_runtime().submit(lazy.collect()).get(),
            common::ErrorCode::InvalidArgument);
    }
    dftu_dataframe* handle = dataframe_handle_wrap(std::move(df));
    CHECK(dftu_dataframe_fill_nan(handle, str_scalar("x")) == nullptr);
    dftu_dataframe* filled = dftu_dataframe_fill_nan(handle, f64_scalar(0.0));
    CHECK(filled != nullptr);
    dftu_dataframe_free(filled);
    dftu_dataframe_free(handle);
}

TEST_CASE("fill_null keeps carrying a text fill over a numeric column") {
    const Floats f;
    const dstd::Vector<std::int64_t> ints{1, 2, 3, 4};
    const dstd::Vector<dstd::StringView> strs{"a", "b", "c", "d"};
    DataFrame df = mixed_frame(f, ints, strs);
    const DataFrame r = df.fill_null(Scalar{str_scalar("x")});
    CHECK(r.columns[0].is_null(1));
}

TEST_CASE("keep_non_nan reports a column it cannot read") {
    dftu_series broken;
    broken.type = TypeId::Float64;
    broken.encoding = Encoding::Selection;
    broken.length = 2;
    std::int64_t nans = 0;
    Series s{new dftu_series(broken)};
    const Series keep = keep_non_nan(s, &nans);
    CHECK_FALSE(keep.valid());
    CHECK(nans == -1);

    DataFrame df;
    df.names = dstd::make_shared_strings({"b"});
    df.columns.push_back(s.share());
    dftu_utils_test::expect_error(df.drop_nans(),
                                  common::ErrorCode::InvalidArgument);
}
