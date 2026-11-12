// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dataframe/expr_ok.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <stdexcept>

using namespace dftracer::utils;

using namespace dftracer::utils::dataframe;

namespace {

// infer_type must agree with what dftu_utils_test::eval_ok() actually produces,
// for every op it covers: build one input column per `input_types` (values are
// irrelevant), evaluate `root` for real, and compare its DataType to
// infer_type's answer.
void check_matches_eval(const Expr& root,
                        const dstd::Vector<DataType>& input_types) {
    dstd::Vector<Series> cols;
    cols.reserve(input_types.size());
    for (const DataType& dt : input_types) {
        switch (dt.id) {
            case TypeId::Int64:
                cols.push_back(Series::flat_i64(
                    dstd::Vector<std::int64_t>{1, 2, 3}.data(), 3));
                break;
            case TypeId::Float64:
                cols.push_back(Series::flat_f64(
                    dstd::Vector<double>{1.0, 2.0, 3.0}.data(), 3));
                break;
            case TypeId::String:
                cols.push_back(Series::strings({"Ab", "Cd", "Ef"}));
                break;
            case TypeId::Bool: {
                dstd::Vector<std::uint8_t> bits{0b101};
                cols.push_back(Series::flat(TypeId::Bool, bits.data(), 3));
                break;
            }
            default:
                FAIL("unsupported type in check_matches_eval helper");
        }
    }
    dstd::Vector<const Series*> ptrs;
    for (const Series& c : cols) ptrs.push_back(&c);

    DataType inferred = dftu_utils_test::infer_type_ok(root, input_types);
    Series result = dftu_utils_test::eval_ok(root, ptrs);
    CHECK(inferred == result.data_type());
}

}  // namespace

TEST_SUITE("expr infer_type") {
    TEST_CASE("bare column reference reports the full input DataType") {
        DataType ts = timestamp(TimeUnit::Micro, "UTC");
        DataType out = dftu_utils_test::infer_type_ok(col(0), {ts});
        CHECK(out == ts);

        DataType dec = decimal128(38, 9);
        CHECK(dftu_utils_test::infer_type_ok(col(0), {dec}) == dec);
    }

    TEST_CASE("arithmetic: int+int stays Int64, any float widens to Float64") {
        check_matches_eval(col(0) + col(1),
                           {scalar(TypeId::Int64), scalar(TypeId::Int64)});
        check_matches_eval(col(0) + col(1),
                           {scalar(TypeId::Int64), scalar(TypeId::Float64)});
        check_matches_eval(col(0) / col(1),
                           {scalar(TypeId::Int64), scalar(TypeId::Int64)});
    }

    TEST_CASE("comparison and logical ops report Bool") {
        check_matches_eval(col(0) > std::int64_t{1}, {scalar(TypeId::Int64)});
        check_matches_eval(
            (col(0) > std::int64_t{1}) & (col(0) < std::int64_t{3}),
            {scalar(TypeId::Int64)});
        check_matches_eval(~(col(0) > std::int64_t{1}),
                           {scalar(TypeId::Int64)});
    }

    TEST_CASE("cast reports the requested target type") {
        DataType out = dftu_utils_test::infer_type_ok(
            expr_cast(TypeId::Float64, col(0)), {scalar(TypeId::Int64)});
        CHECK(out == scalar(TypeId::Float64));
        check_matches_eval(expr_cast(TypeId::Float64, col(0)),
                           {scalar(TypeId::Int64)});
    }

    TEST_CASE("lower reports String") {
        check_matches_eval(expr_lower(col(0)), {scalar(TypeId::String)});
    }

    TEST_CASE(
        "prim reports Int64, unary keeps or widens as "
        "dftu_utils_test::eval_ok() does") {
        check_matches_eval(expr_prim(PrimOp::Ilog2, col(0)),
                           {scalar(TypeId::Int64)});
        check_matches_eval(expr_unary(UnaryOp::Abs, col(0)),
                           {scalar(TypeId::Int64)});
        check_matches_eval(expr_unary(UnaryOp::Sqrt, col(0)),
                           {scalar(TypeId::Int64)});
        check_matches_eval(expr_unary(UnaryOp::IsNan, col(0)),
                           {scalar(TypeId::Float64)});
    }

    TEST_CASE("clip and fillna keep the TypeId but not nested parameters") {
        // alloc_like() (elementwise.cpp) copies only the TypeId, so these
        // kernels drop a Duration's time_unit; infer_type matches that.
        DataType dur = duration_of(TimeUnit::Nano);
        Scalar lo = to_scalar(std::int64_t{0});
        Scalar hi = to_scalar(std::int64_t{100});
        CHECK(
            dftu_utils_test::infer_type_ok(expr_clip(col(0), lo, hi), {dur}) ==
            scalar(TypeId::Duration));
        CHECK(dftu_utils_test::infer_type_ok(expr_fillna(col(0), lo), {dur}) ==
              scalar(TypeId::Duration));
    }

    TEST_CASE("an Unknown input column propagates Unknown, not a guess") {
        DataType unk = scalar(TypeId::Unknown);
        CHECK(dftu_utils_test::infer_type_ok(col(0), {unk}).id ==
              TypeId::Unknown);
        CHECK(dftu_utils_test::infer_type_ok(col(0) + col(1),
                                             {scalar(TypeId::Int64), unk})
                  .id == TypeId::Unknown);
        CHECK(dftu_utils_test::infer_type_ok(col(0) > std::int64_t{1}, {unk})
                  .id == TypeId::Unknown);
        CHECK(dftu_utils_test::infer_type_ok(expr_lower(col(0)), {unk}).id ==
              TypeId::Unknown);
    }

    TEST_CASE("a constant is a column of its type") {
        CHECK(dftu_utils_test::infer_type_ok(lit(std::int64_t{1}),
                                             {scalar(TypeId::Int64)})
                  .id == TypeId::Int64);
        CHECK(dftu_utils_test::infer_type_ok(lit(2.5), {scalar(TypeId::Int64)})
                  .id == TypeId::Float64);
    }

    TEST_CASE("malformed expressions are refused, matching eval()'s contract") {
        CHECK(
            dftu_utils_test::is_invalid(dftracer::utils::dataframe::infer_type(
                col(5), {scalar(TypeId::Int64)})));
        CHECK(
            dftu_utils_test::is_invalid(dftracer::utils::dataframe::infer_type(
                expr_lower(col(0)), {scalar(TypeId::Int64)})));
    }
}
