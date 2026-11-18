// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

constexpr double INF = std::numeric_limits<double>::infinity();
constexpr double NAN_D = std::numeric_limits<double>::quiet_NaN();

using Method = Series (Series::*)() const;

struct MathOp {
    const char* name;
    Method method;
    double (*f64)(double);
    float (*f32)(float);
};

#define MATH_OP(n, expr)                                              \
    MathOp {                                                          \
        #n, &Series::n, [](double x) -> double { return expr; },      \
            [](float x) -> float { return static_cast<float>(expr); } \
    }

const MathOp MATH_OPS[] = {
    MATH_OP(sin, std::sin(x)),
    MATH_OP(cos, std::cos(x)),
    MATH_OP(tan, std::tan(x)),
    MATH_OP(cot, static_cast<decltype(x)>(1) / std::tan(x)),
    MATH_OP(arcsin, std::asin(x)),
    MATH_OP(arccos, std::acos(x)),
    MATH_OP(arctan, std::atan(x)),
    MATH_OP(sinh, std::sinh(x)),
    MATH_OP(cosh, std::cosh(x)),
    MATH_OP(tanh, std::tanh(x)),
    MATH_OP(arcsinh, std::asinh(x)),
    MATH_OP(arccosh, std::acosh(x)),
    MATH_OP(arctanh, std::atanh(x)),
    MATH_OP(cbrt, std::cbrt(x)),
    MATH_OP(log10, std::log10(x)),
    MATH_OP(erf, std::erf(x)),
    MATH_OP(erfc, std::erfc(x)),
};

template <class T>
bool same_float(T a, T b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    if (std::isinf(a) || std::isinf(b)) return a == b;
    return std::abs(a - b) <=
           static_cast<T>(1e-5) * std::max<T>(1, std::abs(b));
}

}  // namespace

TEST_SUITE("math_bitwise") {
    TEST_CASE("math kernels over f64 match libm at edges, NaN and inf") {
        const double in[] = {0.0,  -0.0,   0.5,   -0.5, 1.0,  -1.0,  2.0,
                             -2.0, 1e-300, 1e300, INF,  -INF, NAN_D, 7.0};
        const std::int64_t n = sizeof(in) / sizeof(in[0]);
        const Series s = Series::flat_f64(in, n);
        for (const MathOp& op : MATH_OPS) {
            CAPTURE(op.name);
            const Series r = (s.*op.method)();
            REQUIRE(r.valid());
            CHECK(r.type() == TypeId::Float64);
            REQUIRE(r.length() == n);
            CHECK(r.null_count() == 0);
            for (std::int64_t i = 0; i < n; ++i) {
                CAPTURE(in[i]);
                CHECK(same_float(r.data<double>()[i], op.f64(in[i])));
            }
        }
    }

    TEST_CASE("math kernels keep a Float32 column Float32") {
        const float in[] = {0.0f,
                            0.5f,
                            -0.5f,
                            1.0f,
                            2.0f,
                            -3.0f,
                            std::numeric_limits<float>::infinity(),
                            NAN};
        const std::int64_t n = sizeof(in) / sizeof(in[0]);
        const Series s = Series::flat(TypeId::Float32, in, n);
        for (const MathOp& op : MATH_OPS) {
            CAPTURE(op.name);
            const Series r = (s.*op.method)();
            REQUIRE(r.valid());
            CHECK(r.type() == TypeId::Float32);
            for (std::int64_t i = 0; i < n; ++i) {
                CAPTURE(in[i]);
                CHECK(same_float(r.data<float>()[i], op.f32(in[i])));
            }
        }
    }

    TEST_CASE("math kernels promote every integer width to Float64") {
        auto run = [](TypeId type, const auto* in) {
            const std::int64_t n = 4;
            const Series s = Series::flat(type, in, n);
            for (const MathOp& op : MATH_OPS) {
                CAPTURE(op.name);
                const Series r = (s.*op.method)();
                REQUIRE(r.valid());
                CHECK(r.type() == TypeId::Float64);
                for (std::int64_t i = 0; i < n; ++i)
                    CHECK(same_float(r.data<double>()[i],
                                     op.f64(static_cast<double>(in[i]))));
            }
        };
        const std::int8_t i8[] = {0, 1, 2, -3};
        const std::int16_t i16[] = {0, 1, 2, -3};
        const std::int32_t i32[] = {0, 1, 2, -3};
        const std::int64_t i64[] = {0, 1, 2, -3};
        const std::uint8_t u8[] = {0, 1, 2, 200};
        const std::uint16_t u16[] = {0, 1, 2, 60000};
        const std::uint32_t u32[] = {0, 1, 2, 4000000000u};
        const std::uint64_t u64[] = {0, 1, 2, 18000000000000000000ULL};
        run(TypeId::Int8, i8);
        run(TypeId::Int16, i16);
        run(TypeId::Int32, i32);
        run(TypeId::Int64, i64);
        run(TypeId::Uint8, u8);
        run(TypeId::Uint16, u16);
        run(TypeId::Uint32, u32);
        run(TypeId::Uint64, u64);
    }

    TEST_CASE("math kernels keep nulls and refuse text and Bool") {
        const double in[] = {0.5, 1.0, 2.0, 3.0};
        const std::uint8_t valid[1] = {0b0000'0101};
        const Series s = Series::flat_f64(in, 4, valid);
        for (const MathOp& op : MATH_OPS) {
            CAPTURE(op.name);
            const Series r = (s.*op.method)();
            REQUIRE(r.valid());
            CHECK(r.null_count() == 2);
            CHECK_FALSE(r.is_null(0));
            CHECK(r.is_null(1));
            CHECK_FALSE(r.is_null(2));
            CHECK(r.is_null(3));
            const std::uint8_t bits[1] = {0b0000'0101};
            CHECK_FALSE(
                (Series::flat(TypeId::Bool, bits, 3).*op.method)().valid());
        }
    }

    TEST_CASE("arcsin of two is NaN and log10 of a power of ten is exact") {
        const double in[] = {2.0, 1.0, 1000.0, 1e15};
        const Series s = Series::flat_f64(in, 4);
        CHECK(std::isnan(s.arcsin().data<double>()[0]));
        CHECK(s.log10().data<double>()[2] == 3.0);
        CHECK(s.log10().data<double>()[3] == 15.0);
    }

    template <class T>
    std::uint32_t ref_count(T v, int kind) {
        using U = std::make_unsigned_t<T>;
        const U u = static_cast<U>(v);
        const auto w = static_cast<std::uint32_t>(sizeof(U) * 8);
        switch (kind) {
            case 0:
                return std::popcount(u);
            case 1:
                return w - std::popcount(u);
            case 2:
                return std::countl_one(u);
            case 3:
                return std::countl_zero(u);
            case 4:
                return std::countr_one(u);
            default:
                return std::countr_zero(u);
        }
    }

    using Count = Series (Series::*)() const;
    const Count COUNTS[] = {
        &Series::bitwise_count_ones,    &Series::bitwise_count_zeros,
        &Series::bitwise_leading_ones,  &Series::bitwise_leading_zeros,
        &Series::bitwise_trailing_ones, &Series::bitwise_trailing_zeros};

    template <class T>
    void check_counts(TypeId type) {
        constexpr T lo = std::numeric_limits<T>::min();
        constexpr T hi = std::numeric_limits<T>::max();
        const T in[] = {T(0),
                        T(1),
                        T(5),
                        T(6),
                        hi,
                        lo,
                        static_cast<T>(~T(0)),
                        T(8),
                        static_cast<T>(lo + 1)};
        const std::int64_t n = sizeof(in) / sizeof(in[0]);
        const std::uint8_t valid[2] = {0b1111'0111, 0b0000'0001};
        const Series s = Series::flat(type, in, n, valid);
        for (int kind = 0; kind < 6; ++kind) {
            const Series r = (s.*COUNTS[kind])();
            REQUIRE(r.valid());
            CHECK(r.type() == TypeId::Uint32);
            CHECK(r.null_count() == 1);
            CHECK(r.is_null(3));
            for (std::int64_t i = 0; i < n; ++i) {
                if (i == 3) continue;
                CAPTURE(kind);
                CAPTURE(i);
                CHECK(r.data<std::uint32_t>()[i] == ref_count(in[i], kind));
            }
        }
    }

    TEST_CASE("bit counts are exact for every integer width") {
        check_counts<std::int8_t>(TypeId::Int8);
        check_counts<std::int16_t>(TypeId::Int16);
        check_counts<std::int32_t>(TypeId::Int32);
        check_counts<std::int64_t>(TypeId::Int64);
        check_counts<std::uint8_t>(TypeId::Uint8);
        check_counts<std::uint16_t>(TypeId::Uint16);
        check_counts<std::uint32_t>(TypeId::Uint32);
        check_counts<std::uint64_t>(TypeId::Uint64);
    }

    TEST_CASE("bit counts of known values") {
        const std::int8_t in[] = {-1, -8, 0, 1};
        const Series s = Series::flat(TypeId::Int8, in, 4);
        const std::uint32_t ones[] = {8, 5, 0, 1};
        const std::uint32_t leading_ones[] = {8, 5, 0, 0};
        const std::uint32_t trailing_zeros[] = {0, 3, 8, 0};
        const Series a = s.bitwise_count_ones();
        const Series b = s.bitwise_leading_ones();
        const Series c = s.bitwise_trailing_zeros();
        for (int i = 0; i < 4; ++i) {
            CHECK(a.data<std::uint32_t>()[i] == ones[i]);
            CHECK(b.data<std::uint32_t>()[i] == leading_ones[i]);
            CHECK(c.data<std::uint32_t>()[i] == trailing_zeros[i]);
        }
    }

    TEST_CASE("bit counts of a Bool column and refusal of floats and text") {
        const std::uint8_t bits[1] = {0b0000'0101};
        const std::uint8_t valid[1] = {0b0000'0111};
        const Series b = Series::flat(TypeId::Bool, bits, 3, valid);
        const std::uint32_t want[6][3] = {{1, 0, 1}, {0, 1, 0}, {1, 0, 1},
                                          {0, 1, 0}, {1, 0, 1}, {0, 1, 0}};
        for (int kind = 0; kind < 6; ++kind) {
            const Series r = (b.*COUNTS[kind])();
            REQUIRE(r.valid());
            CHECK(r.type() == TypeId::Uint32);
            for (int i = 0; i < 3; ++i)
                CHECK(r.data<std::uint32_t>()[i] == want[kind][i]);
        }
        const double f[] = {1.0};
        for (const Count c : COUNTS)
            CHECK_FALSE((Series::flat_f64(f, 1).*c)().valid());
    }

    template <class T>
    void check_reductions(TypeId type) {
        constexpr bool is_signed = std::is_signed_v<T>;
        const T in[] = {T(6), T(3), static_cast<T>(~T(0)), T(12), T(9)};
        const std::uint8_t valid[1] = {0b0001'1011};
        const Series s = Series::flat(type, in, 5, valid);
        T a = in[0], o = in[0], x = in[0];
        for (int i : {1, 3, 4}) {
            a = static_cast<T>(a & in[i]);
            o = static_cast<T>(o | in[i]);
            x = static_cast<T>(x ^ in[i]);
        }
        auto value = [](dftu_scalar v)
            -> std::conditional_t<is_signed, std::int64_t, std::uint64_t> {
            if constexpr (is_signed) {
                CHECK(v.kind == DFTU_SCALAR_TAG_I64);
                return v.value.i;
            } else {
                CHECK(v.kind == DFTU_SCALAR_TAG_U64);
                return v.value.u;
            }
        };
        CHECK(value(s.bitwise_and().data) == a);
        CHECK(value(s.bitwise_or().data) == o);
        CHECK(value(s.bitwise_xor().data) == x);

        const std::uint8_t none[1] = {0};
        const Series empty = Series::flat(type, in, 5, none);
        CHECK(empty.bitwise_and().data.kind == DFTU_SCALAR_TAG_NULL);
        CHECK(empty.bitwise_or().data.kind == DFTU_SCALAR_TAG_NULL);
        CHECK(empty.bitwise_xor().data.kind == DFTU_SCALAR_TAG_NULL);
        CHECK(s.head(0).bitwise_xor().data.kind == DFTU_SCALAR_TAG_NULL);
    }

    TEST_CASE(
        "bitwise reductions keep the sign of the type and null an empty "
        "column") {
        check_reductions<std::int8_t>(TypeId::Int8);
        check_reductions<std::int16_t>(TypeId::Int16);
        check_reductions<std::int32_t>(TypeId::Int32);
        check_reductions<std::int64_t>(TypeId::Int64);
        check_reductions<std::uint8_t>(TypeId::Uint8);
        check_reductions<std::uint16_t>(TypeId::Uint16);
        check_reductions<std::uint32_t>(TypeId::Uint32);
        check_reductions<std::uint64_t>(TypeId::Uint64);
    }

    TEST_CASE("bitwise reductions over Bool, floats and text") {
        const std::uint8_t bits[1] = {0b0000'1101};
        const std::uint8_t valid[1] = {0b0000'1011};
        const Series b = Series::flat(TypeId::Bool, bits, 4, valid);
        CHECK(b.bitwise_and().data.value.i == 0);
        CHECK(b.bitwise_or().data.value.i == 1);
        CHECK(b.bitwise_xor().data.value.i == 0);
        CHECK(b.bitwise_xor().data.kind == DFTU_SCALAR_TAG_I64);
        const double f[] = {1.0, 2.0};
        const Series fs = Series::flat_f64(f, 2);
        CHECK(fs.bitwise_and().data.kind == DFTU_SCALAR_TAG_ERR);
        CHECK(fs.bitwise_or().data.kind == DFTU_SCALAR_TAG_ERR);
        CHECK(fs.bitwise_xor().data.kind == DFTU_SCALAR_TAG_ERR);
    }
}
