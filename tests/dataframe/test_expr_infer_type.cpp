#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace dftracer::utils::dataframe;

namespace {

// infer_type must agree with what eval() actually produces, for every op it
// covers: build one input column per `input_types` (values are irrelevant),
// evaluate `root` for real, and compare its DataType to infer_type's answer.
void check_matches_eval(const Expr& root,
                        const std::vector<DataType>& input_types) {
    std::vector<Series> cols;
    cols.reserve(input_types.size());
    for (const DataType& dt : input_types) {
        switch (dt.id) {
            case TypeId::Int64:
                cols.push_back(Series::flat_i64(
                    std::vector<std::int64_t>{1, 2, 3}.data(), 3));
                break;
            case TypeId::Float64:
                cols.push_back(Series::flat_f64(
                    std::vector<double>{1.0, 2.0, 3.0}.data(), 3));
                break;
            case TypeId::String:
                cols.push_back(Series::strings({"Ab", "Cd", "Ef"}));
                break;
            case TypeId::Bool: {
                std::vector<std::uint8_t> bits{0b101};
                cols.push_back(Series::flat(TypeId::Bool, bits.data(), 3));
                break;
            }
            default:
                FAIL("unsupported type in check_matches_eval helper");
        }
    }
    std::vector<const Series*> ptrs;
    for (const Series& c : cols) ptrs.push_back(&c);

    DataType inferred = infer_type(root, input_types);
    Series result = eval(root, ptrs);
    CHECK(inferred == result.data_type());
}

}  // namespace

TEST_SUITE("expr infer_type") {
    TEST_CASE("bare column reference reports the full input DataType") {
        DataType ts = timestamp(TimeUnit::Micro, "UTC");
        DataType out = infer_type(col(0), {ts});
        CHECK(out == ts);

        DataType dec = decimal128(38, 9);
        CHECK(infer_type(col(0), {dec}) == dec);
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
        DataType out = infer_type(expr_cast(TypeId::Float64, col(0)),
                                  {scalar(TypeId::Int64)});
        CHECK(out == scalar(TypeId::Float64));
        check_matches_eval(expr_cast(TypeId::Float64, col(0)),
                           {scalar(TypeId::Int64)});
    }

    TEST_CASE("lower reports String") {
        check_matches_eval(expr_lower(col(0)), {scalar(TypeId::String)});
    }

    TEST_CASE("prim reports Int64, unary keeps or widens as eval() does") {
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
        CHECK(infer_type(expr_clip(col(0), lo, hi), {dur}) ==
              scalar(TypeId::Duration));
        CHECK(infer_type(expr_fillna(col(0), lo), {dur}) ==
              scalar(TypeId::Duration));
    }

    TEST_CASE("an Unknown input column propagates Unknown, not a guess") {
        DataType unk = scalar(TypeId::Unknown);
        CHECK(infer_type(col(0), {unk}).id == TypeId::Unknown);
        CHECK(infer_type(col(0) + col(1), {scalar(TypeId::Int64), unk}).id ==
              TypeId::Unknown);
        CHECK(infer_type(col(0) > std::int64_t{1}, {unk}).id ==
              TypeId::Unknown);
        CHECK(infer_type(expr_lower(col(0)), {unk}).id == TypeId::Unknown);
    }

    TEST_CASE("malformed expressions throw, matching eval()'s contract") {
        CHECK_THROWS_AS(infer_type(col(5), {scalar(TypeId::Int64)}),
                        std::invalid_argument);
        CHECK_THROWS_AS(
            infer_type(lit(std::int64_t{1}), {scalar(TypeId::Int64)}),
            std::invalid_argument);
        CHECK_THROWS_AS(infer_type(expr_lower(col(0)), {scalar(TypeId::Int64)}),
                        std::invalid_argument);
    }
}
