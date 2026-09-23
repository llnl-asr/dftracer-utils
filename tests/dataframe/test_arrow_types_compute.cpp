#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// Confirms the new Arrow logical types compute, not just round-trip: Float16
// arithmetic/reduction promote to Float32 and produce real numbers,
// FixedSizeBinary/Decimal128 compare/sort/group_by/is_in on their exact
// bytes, and Decimal128 arithmetic goes through the documented (lossy)
// Float64 cast.
#ifdef DFTRACER_UTILS_ENABLE_ARROW
// clang-format off
// nanoarrow.h must precede arrow.h: arrow.h (via plugins/arrow_abi.h) defines
// ArrowSchema/ArrowArray under the ARROW_C_DATA_INTERFACE guard but not
// ArrowArrayStream, which nanoarrow.h's inline helpers then need complete.
#include <nanoarrow/nanoarrow.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/arrow.h>
#include <dftracer/utils/dataframe/internal/float16.h>
#include <dftracer/utils/dataframe/scalar.h>
// clang-format on

namespace df = dftracer::utils::dataframe;

using dftracer::utils::dataframe::Agg;
using dftracer::utils::dataframe::AggOp;
using dftracer::utils::dataframe::AggSpec;
using dftracer::utils::dataframe::AggStatePtr;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::DataType;
using dftracer::utils::dataframe::fixed_size_binary;
using dftracer::utils::dataframe::GroupAgg;
using dftracer::utils::dataframe::OwnedArrow;
using dftracer::utils::dataframe::Scalar;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;

namespace {

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

Series make_float16(const std::vector<float>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetType(&b.schema, NANOARROW_TYPE_HALF_FLOAT) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (float v : values)
        REQUIRE(ArrowArrayAppendDouble(&b.array, static_cast<double>(v)) ==
                NANOARROW_OK);
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_fixed_size_binary(const std::vector<std::string>& values,
                              std::int32_t width) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeFixedSize(&b.schema,
                                        NANOARROW_TYPE_FIXED_SIZE_BINARY,
                                        width) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (const std::string& v : values) {
        REQUIRE(static_cast<std::int32_t>(v.size()) == width);
        ArrowBufferView bv;
        bv.data.data = v.data();
        bv.size_bytes = width;
        REQUIRE(ArrowArrayAppendBytes(&b.array, bv) == NANOARROW_OK);
    }
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_binary(const std::vector<std::string>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetType(&b.schema, NANOARROW_TYPE_BINARY) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (const std::string& v : values) {
        ArrowBufferView bv;
        bv.data.data = v.data();
        bv.size_bytes = static_cast<int64_t>(v.size());
        REQUIRE(ArrowArrayAppendBytes(&b.array, bv) == NANOARROW_OK);
    }
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_large_binary(const std::vector<std::string>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetType(&b.schema, NANOARROW_TYPE_LARGE_BINARY) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (const std::string& v : values) {
        ArrowBufferView bv;
        bv.data.data = v.data();
        bv.size_bytes = static_cast<int64_t>(v.size());
        REQUIRE(ArrowArrayAppendBytes(&b.array, bv) == NANOARROW_OK);
    }
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_timestamp(const std::vector<std::int64_t>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_TIMESTAMP,
                                       NANOARROW_TIME_UNIT_MICRO,
                                       "UTC") == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_decimal128(const std::vector<std::int64_t>& values,
                       std::int32_t precision = 38, std::int32_t scale = 0) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDecimal(&b.schema, NANOARROW_TYPE_DECIMAL128,
                                      precision, scale) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values) {
        struct ArrowDecimal dec;
        ArrowDecimalInit(&dec, 128, precision, scale);
        ArrowDecimalSetInt(&dec, v);
        REQUIRE(ArrowArrayAppendDecimal(&b.array, &dec) == NANOARROW_OK);
    }
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_decimal256(const std::vector<std::int64_t>& values,
                       std::int32_t precision, std::int32_t scale) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDecimal(&b.schema, NANOARROW_TYPE_DECIMAL256,
                                      precision, scale) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values) {
        struct ArrowDecimal dec;
        ArrowDecimalInit(&dec, 256, precision, scale);
        ArrowDecimalSetInt(&dec, v);
        REQUIRE(ArrowArrayAppendDecimal(&b.array, &dec) == NANOARROW_OK);
    }
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_timestamp_tz(ArrowTimeUnit unit, const char* tz,
                         const std::vector<std::int64_t>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_TIMESTAMP,
                                       unit, tz) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_duration(ArrowTimeUnit unit,
                     const std::vector<std::int64_t>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_DURATION, unit,
                                       nullptr) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_time64(ArrowTimeUnit unit,
                   const std::vector<std::int64_t>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_TIME64, unit,
                                       nullptr) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

}  // namespace

TEST_SUITE("dataframe_arrow_types_compute") {
    TEST_CASE("Float16 arithmetic promotes to Float32 and computes exactly") {
        Series a = make_float16({1.5f, 2.5f, -3.0f});
        Series b = make_float16({1.0f, 1.0f, 1.0f});
        REQUIRE(a.valid());
        REQUIRE(b.valid());

        Series sum = a.add(b);
        REQUIRE(sum.valid());
        CHECK(sum.type() == TypeId::Float32);
        const float* d = sum.data<float>();
        REQUIRE(d != nullptr);
        CHECK(d[0] == doctest::Approx(2.5f));
        CHECK(d[1] == doctest::Approx(3.5f));
        CHECK(d[2] == doctest::Approx(-2.0f));
    }

    TEST_CASE("Float16 reduction (sum) promotes to Float32 and computes") {
        Series a = make_float16({1.0f, 2.0f, 3.0f});
        REQUIRE(a.valid());
        Scalar total = a.sum();
        CHECK(total.f64() == doctest::Approx(6.0));
    }

    TEST_CASE("FixedSizeBinary sorts by exact byte order") {
        Series s = make_fixed_size_binary({"bbbb", "aaaa", "cccc", "aaaa"}, 4);
        REQUIRE(s.valid());
        Series sorted = s.sort(false);
        REQUIRE(sorted.valid());
        REQUIRE(sorted.length() == 4);
        const char* d = sorted.data<char>();
        REQUIRE(d != nullptr);
        CHECK(std::string(d, 4) == "aaaa");
        CHECK(std::string(d + 4, 4) == "aaaa");
        CHECK(std::string(d + 8, 4) == "bbbb");
        CHECK(std::string(d + 12, 4) == "cccc");
    }

    TEST_CASE("FixedSizeBinary is_in matches on exact bytes") {
        Series s = make_fixed_size_binary({"aaaa", "bbbb", "cccc"}, 4);
        Series needles = make_fixed_size_binary({"bbbb"}, 4);
        REQUIRE(s.valid());
        REQUIRE(needles.valid());
        Series mask = s.is_in(needles);
        REQUIRE(mask.valid());
        REQUIRE(mask.length() == 3);
        CHECK(mask.is_null(0) == false);
        const std::uint8_t* bits = mask.data<std::uint8_t>();
        REQUIRE(bits != nullptr);
        auto bit = [&](std::int64_t i) {
            return ((bits[i >> 3] >> (i & 7)) & 1) != 0;
        };
        CHECK(bit(0) == false);
        CHECK(bit(1) == true);
        CHECK(bit(2) == false);
    }

    TEST_CASE("FixedSizeBinary groups by exact byte equality") {
        Series keys = make_fixed_size_binary(
            {"aaaa", "bbbb", "aaaa", "cccc", "bbbb", "aaaa"}, 4);
        std::vector<std::int64_t> ones = {1, 1, 1, 1, 1, 1};
        Series values = Series::flat_i64(ones.data(), 6);
        REQUIRE(keys.valid());
        REQUIRE(values.valid());

        dftu_series* out_keys = nullptr;
        dftu_series* out_values[5] = {};
        std::int32_t n =
            dftu_series_group_by(keys.handle(), values.handle(),
                                 DFTU_REDUCE_COUNT, &out_keys, out_values, 5);
        REQUIRE(n == 1);
        REQUIRE(out_keys != nullptr);
        CHECK(dftu_series_length(out_keys) == 3);  // aaaa, bbbb, cccc
        Series counts{out_values[0]};
        REQUIRE(counts.valid());
        REQUIRE(counts.length() == 3);
        const std::int64_t* c = counts.data<std::int64_t>();
        REQUIRE(c != nullptr);
        CHECK(c[0] == 3);  // aaaa first-seen
        CHECK(c[1] == 2);  // bbbb
        CHECK(c[2] == 1);  // cccc
        dftu_series_free(out_keys);
    }

    TEST_CASE("Decimal128 sorts by exact numeric order, not byte order") {
        // -1 and 300 both have a nonzero low byte; only a signed 128-bit
        // compare (not memcmp) orders them correctly.
        Series s = make_decimal128({300, -1, 100});
        REQUIRE(s.valid());
        Series sorted = s.sort(false);
        REQUIRE(sorted.valid());
        REQUIRE(sorted.length() == 3);
        const std::uint8_t* d = sorted.data<std::uint8_t>();
        REQUIRE(d != nullptr);
        std::int64_t v0, v1, v2;
        std::memcpy(&v0, d, 8);
        std::memcpy(&v1, d + 16, 8);
        std::memcpy(&v2, d + 32, 8);
        CHECK(v0 == -1);
        CHECK(v1 == 100);
        CHECK(v2 == 300);
    }

    TEST_CASE("Decimal128 groups by exact value") {
        Series keys = make_decimal128({100, 300, 100});
        std::vector<std::int64_t> ones = {1, 1, 1};
        Series values = Series::flat_i64(ones.data(), 3);
        REQUIRE(keys.valid());

        dftu_series* out_keys = nullptr;
        dftu_series* out_values[5] = {};
        std::int32_t n =
            dftu_series_group_by(keys.handle(), values.handle(),
                                 DFTU_REDUCE_COUNT, &out_keys, out_values, 5);
        REQUIRE(n == 1);
        REQUIRE(dftu_series_length(out_keys) == 2);
        Series counts{out_values[0]};
        REQUIRE(counts.length() == 2);
        const std::int64_t* c = counts.data<std::int64_t>();
        CHECK(c[0] == 2);  // 100 first-seen
        CHECK(c[1] == 1);  // 300
        dftu_series_free(out_keys);
    }

    TEST_CASE("Decimal128 + Decimal128 same scale is exact, not Float64") {
        Series a = make_decimal128({100, 200});  // scale 0
        Series b = make_decimal128({1, 1});
        REQUIRE(a.valid());
        Series sum = a.add(b);
        REQUIRE(sum.valid());
        CHECK(sum.type() == TypeId::Decimal128);
        CHECK(sum.data_type().decimal_scale == 0);
        const std::uint8_t* d = sum.data<std::uint8_t>();
        REQUIRE(d != nullptr);
        std::int64_t v0, v1;
        std::memcpy(&v0, d, 8);
        std::memcpy(&v1, d + 16, 8);
        CHECK(v0 == 101);
        CHECK(v1 == 201);
    }

    TEST_CASE(
        "Decimal128 arithmetic against a plain scalar still converts "
        "to Float64 (documented precision loss)") {
        Series a = make_decimal128({100, 200});
        dftu_scalar one{};
        one.kind = DFTU_SCALAR_TAG_I64;
        one.value.i = 1;
        dftu_series* sum_raw = dftu_series_add_scalar(a.handle(), one);
        REQUIRE(sum_raw != nullptr);
        Series sum{sum_raw};
        CHECK(sum.type() == TypeId::Float64);
        const double* d = sum.data<double>();
        REQUIRE(d != nullptr);
        CHECK(d[0] == doctest::Approx(101.0));
        CHECK(d[1] == doctest::Approx(201.0));
    }

    TEST_CASE("Decimal128 add rescales the lower-scale operand exactly") {
        // 1.50 (scale 2) + 1.5 (scale 1) = 3.00 (scale 2): the scale-1
        // operand's 15 must become 150, not just be added as 15.
        Series a = make_decimal128({150}, 10, 2);
        Series b = make_decimal128({15}, 10, 1);
        REQUIRE(a.valid());
        REQUIRE(b.valid());
        Series sum = a.add(b);
        REQUIRE(sum.valid());
        CHECK(sum.type() == TypeId::Decimal128);
        CHECK(sum.data_type().decimal_scale == 2);
        std::int64_t v0;
        std::memcpy(&v0, sum.data<std::uint8_t>(), 8);
        CHECK(v0 == 300);  // 3.00
    }

    TEST_CASE("Decimal128 subtract rescales and computes exactly") {
        // 5.00 (scale 2) - 1.5 (scale 1) = 3.50 (scale 2).
        Series a = make_decimal128({500}, 10, 2);
        Series b = make_decimal128({15}, 10, 1);
        Series diff = a.sub(b);
        REQUIRE(diff.valid());
        CHECK(diff.data_type().decimal_scale == 2);
        std::int64_t v0;
        std::memcpy(&v0, diff.data<std::uint8_t>(), 8);
        CHECK(v0 == 350);  // 3.50
    }

    TEST_CASE("Decimal128 multiply adds the scales exactly") {
        // 1.25 (scale 2) * 2.0 (scale 1) = 2.500 (scale 3).
        Series a = make_decimal128({125}, 10, 2);
        Series b = make_decimal128({20}, 10, 1);
        Series prod = a.mul(b);
        REQUIRE(prod.valid());
        CHECK(prod.type() == TypeId::Decimal128);
        CHECK(prod.data_type().decimal_scale == 3);
        std::int64_t v0;
        std::memcpy(&v0, prod.data<std::uint8_t>(), 8);
        CHECK(v0 == 2500);  // 2.500
    }

    TEST_CASE("Decimal128 divide truncates at the fixed scale increment") {
        // 10.00 (scale 2) / 3.0 (scale 1), 4-digit scale increment:
        // result scale = 2 + 4 = 6; 10 / 3 = 3.333333... truncated to
        // 3.333333 -> raw 3333333.
        Series a = make_decimal128({1000}, 20, 2);
        Series b = make_decimal128({30}, 20, 1);
        Series quot = a.div(b);
        REQUIRE(quot.valid());
        CHECK(quot.data_type().decimal_scale == 6);
        std::int64_t v0;
        std::memcpy(&v0, quot.data<std::uint8_t>(), 8);
        CHECK(v0 == 3333333);
    }

    TEST_CASE(
        "Decimal128 add overflowing its declared precision refuses "
        "loudly, not a wrapped value") {
        // A raw value already far past what "precision 3" can hold (the
        // engine trusts the caller's declared precision, same as Arrow).
        Series a = make_decimal128({999999999999LL}, 3, 0);
        Series b = make_decimal128({999999999999LL}, 3, 0);
        Series sum = a.add(b);
        CHECK_FALSE(sum.valid());
    }

    TEST_CASE(
        "Decimal128 multiply overflowing its declared precision "
        "refuses loudly") {
        Series a = make_decimal128({999999999LL}, 4, 0);
        Series b = make_decimal128({999999999LL}, 4, 0);
        Series prod = a.mul(b);
        CHECK_FALSE(prod.valid());
    }

    TEST_CASE(
        "Decimal128 add refuses loudly when the scale mismatch "
        "overflows the 128-bit container itself") {
        Series a = make_decimal128({999999999999999999LL}, 38, 0);
        Series b = make_decimal128({999999999999999999LL}, 38, 30);
        Series sum = a.add(b);
        CHECK_FALSE(sum.valid());
    }

    TEST_CASE("Decimal256 + Decimal256 same scale is exact") {
        Series a = make_decimal256({100, 200}, 40, 0);
        Series b = make_decimal256({1, 1}, 40, 0);
        REQUIRE(a.valid());
        Series sum = a.add(b);
        REQUIRE(sum.valid());
        CHECK(sum.type() == TypeId::Decimal256);
        CHECK(sum.data_type().decimal_scale == 0);
        std::int64_t v0, v1;
        std::memcpy(&v0, sum.data<std::uint8_t>(), 8);
        std::memcpy(&v1, sum.data<std::uint8_t>() + 32, 8);
        CHECK(v0 == 101);
        CHECK(v1 == 201);
    }

    TEST_CASE("Decimal256 subtract rescales exactly") {
        Series a = make_decimal256({500}, 40, 2);
        Series b = make_decimal256({15}, 40, 1);
        Series diff = a.sub(b);
        REQUIRE(diff.valid());
        CHECK(diff.data_type().decimal_scale == 2);
        std::int64_t v0;
        std::memcpy(&v0, diff.data<std::uint8_t>(), 8);
        CHECK(v0 == 350);  // 3.50
    }

    TEST_CASE(
        "Decimal256 multiply/divide still fall back to Float64 "
        "(no exact 512-bit path here)") {
        Series a = make_decimal256({100, 200}, 40, 0);
        Series b = make_decimal256({2, 2}, 40, 0);
        Series prod = a.mul(b);
        REQUIRE(prod.valid());
        CHECK(prod.type() == TypeId::Float64);
        const double* d = prod.data<double>();
        REQUIRE(d != nullptr);
        CHECK(d[0] == doctest::Approx(200.0));
        CHECK(d[1] == doctest::Approx(400.0));
    }

    TEST_CASE("Casting INTO a decimal type is refused, not silently wrong") {
        std::vector<double> vals = {1.5, 2.5};
        Series a = Series::flat_f64(vals.data(), 2);
        Series casted = a.cast(TypeId::Decimal128);
        CHECK_FALSE(casted.valid());
    }

    TEST_CASE("Timestamp sorts on its exact physical Int64 value") {
        Series s = make_timestamp({300, -1, 100});
        REQUIRE(s.valid());
        Series sorted = s.sort(false);
        REQUIRE(sorted.valid());
        REQUIRE(sorted.type() ==
                TypeId::Timestamp);  // stays Timestamp, not Int64
        const std::int64_t* d = sorted.data<std::int64_t>();
        REQUIRE(d != nullptr);
        CHECK(d[0] == -1);
        CHECK(d[1] == 100);
        CHECK(d[2] == 300);
    }

    TEST_CASE("Timestamp min/max compute on the exact value") {
        Series s = make_timestamp({300, -1, 100});
        REQUIRE(s.valid());
        Scalar mn = s.min();
        Scalar mx = s.max();
        CHECK(mn.i64() == -1);
        CHECK(mx.i64() == 300);
    }

    TEST_CASE("Timestamp comparison works against a scalar threshold") {
        Series s = make_timestamp({300, -1, 100});
        REQUIRE(s.valid());
        dftu_scalar rhs{};
        rhs.kind = DFTU_SCALAR_TAG_I64;
        rhs.value.i = 100;
        dftu_series* mask = dftu_series_compare(s.handle(), DFTU_CMP_GT, rhs);
        REQUIRE(mask != nullptr);
        Series m{mask};
        REQUIRE(m.length() == 3);
        const std::uint8_t* bits = m.data<std::uint8_t>();
        REQUIRE(bits != nullptr);
        auto bit = [&](std::int64_t i) {
            return ((bits[i >> 3] >> (i & 7)) & 1) != 0;
        };
        CHECK(bit(0) == true);   // 300 > 100
        CHECK(bit(1) == false);  // -1 > 100
        CHECK(bit(2) == false);  // 100 > 100
    }

    TEST_CASE("Timestamp is_in matches on the exact value") {
        Series s = make_timestamp({300, -1, 100});
        Series needles = make_timestamp({100});
        REQUIRE(s.valid());
        Series mask = s.is_in(needles);
        REQUIRE(mask.valid());
        REQUIRE(mask.length() == 3);
        const std::uint8_t* bits = mask.data<std::uint8_t>();
        REQUIRE(bits != nullptr);
        auto bit = [&](std::int64_t i) {
            return ((bits[i >> 3] >> (i & 7)) & 1) != 0;
        };
        CHECK(bit(0) == false);
        CHECK(bit(1) == false);
        CHECK(bit(2) == true);
    }

    TEST_CASE("Timestamp groups on the exact value, via both group_by paths") {
        Series keys = make_timestamp({100, 300, 100});
        std::vector<std::int64_t> ones = {1, 1, 1};
        Series values = Series::flat_i64(ones.data(), 3);
        REQUIRE(keys.valid());

        // Low-level ABI primitive (kernels/group_by.cpp).
        dftu_series* out_keys = nullptr;
        dftu_series* out_values[5] = {};
        std::int32_t n =
            dftu_series_group_by(keys.handle(), values.handle(),
                                 DFTU_REDUCE_COUNT, &out_keys, out_values, 5);
        REQUIRE(n == 1);
        REQUIRE(dftu_series_length(out_keys) == 2);
        Series counts{out_values[0]};
        const std::int64_t* c = counts.data<std::int64_t>();
        CHECK(c[0] == 2);  // 100 first-seen
        CHECK(c[1] == 1);  // 300
        dftu_series_free(out_keys);

        // DataFrame::group_by (AggState/group_agg engine).
        DataFrame df;
        df.names = {"ts", "v"};
        df.columns.push_back(keys.share());
        df.columns.push_back(values.share());
        DataFrame grouped = df.group_by("ts", {GroupAgg{Agg::Count, "v", "n"}});
        REQUIRE(grouped.num_rows() == 2);
        Series gkeys = grouped.column("ts");
        Series gn = grouped.column("n");
        REQUIRE(gkeys.type() == TypeId::Timestamp);
        const std::int64_t* kp = gkeys.data<std::int64_t>();
        const std::int64_t* np = gn.data<std::int64_t>();
        REQUIRE(kp != nullptr);
        REQUIRE(np != nullptr);
        for (std::int64_t i = 0; i < 2; ++i) {
            if (kp[i] == 100)
                CHECK(np[i] == 2);
            else if (kp[i] == 300)
                CHECK(np[i] == 1);
            else
                FAIL("unexpected group key");
        }
    }

    TEST_CASE("Timestamp + Timestamp is refused, not silently an integer") {
        Series a = make_timestamp({100, 200});
        Series b = make_timestamp({1, 1});
        REQUIRE(a.valid());
        Series sum = a.add(b);
        CHECK_FALSE(sum.valid());
    }

    TEST_CASE("Timestamp - Timestamp is an exact Duration at the finer unit") {
        Series a =
            make_timestamp_tz(NANOARROW_TIME_UNIT_MILLI, "UTC", {5000, 9000});
        Series b = make_timestamp_tz(NANOARROW_TIME_UNIT_MICRO, "UTC",
                                     {1000000, 2000000});  // 1s, 2s in micros
        REQUIRE(a.valid());
        REQUIRE(b.valid());
        Series diff = a.sub(b);
        REQUIRE(diff.valid());
        CHECK(diff.type() == TypeId::Duration);
        CHECK(diff.data_type().time_unit ==
              dftracer::utils::dataframe::TimeUnit::Micro);
        const std::int64_t* d = diff.data<std::int64_t>();
        REQUIRE(d != nullptr);
        // 5000ms=5,000,000us - 1,000,000us = 4,000,000us
        CHECK(d[0] == 4000000);
        // 9000ms=9,000,000us - 2,000,000us = 7,000,000us
        CHECK(d[1] == 7000000);
    }

    TEST_CASE(
        "Timestamp - Timestamp with mismatched timezones is refused, "
        "not silently naive-vs-aware") {
        Series a = make_timestamp_tz(NANOARROW_TIME_UNIT_MICRO, "UTC", {100});
        Series b = make_timestamp_tz(NANOARROW_TIME_UNIT_MICRO,
                                     "America/Denver", {50});
        Series diff = a.sub(b);
        CHECK_FALSE(diff.valid());

        Series naive =
            make_timestamp_tz(NANOARROW_TIME_UNIT_MICRO, nullptr, {50});
        Series diff2 = a.sub(naive);
        CHECK_FALSE(diff2.valid());
    }

    TEST_CASE(
        "Timestamp + Duration is an exact Timestamp keeping the "
        "timezone, at the finer unit") {
        Series ts =
            make_timestamp_tz(NANOARROW_TIME_UNIT_SECOND, "UTC", {10, 20});
        Series dur = make_duration(NANOARROW_TIME_UNIT_MILLI, {500, 1500});
        Series sum = ts.add(dur);
        REQUIRE(sum.valid());
        CHECK(sum.type() == TypeId::Timestamp);
        CHECK(sum.data_type().time_unit ==
              dftracer::utils::dataframe::TimeUnit::Milli);
        CHECK(sum.data_type().timezone == "UTC");
        const std::int64_t* d = sum.data<std::int64_t>();
        REQUIRE(d != nullptr);
        CHECK(d[0] == 10500);       // 10s + 500ms
        CHECK(d[1] == 21500);       // 20s + 1500ms

        Series sum2 = dur.add(ts);  // Duration + Timestamp, same result
        REQUIRE(sum2.valid());
        CHECK(sum2.type() == TypeId::Timestamp);
        const std::int64_t* d2 = sum2.data<std::int64_t>();
        CHECK(d2[0] == 10500);
    }

    TEST_CASE("Timestamp - Duration is an exact Timestamp") {
        Series ts = make_timestamp_tz(NANOARROW_TIME_UNIT_SECOND, "UTC", {10});
        Series dur = make_duration(NANOARROW_TIME_UNIT_SECOND, {3});
        Series diff = ts.sub(dur);
        REQUIRE(diff.valid());
        CHECK(diff.type() == TypeId::Timestamp);
        CHECK(diff.data<std::int64_t>()[0] == 7);
    }

    TEST_CASE("Duration - Timestamp is refused (wrong order)") {
        Series ts = make_timestamp({10});
        Series dur = make_duration(NANOARROW_TIME_UNIT_MICRO, {3});
        Series diff = dur.sub(ts);
        CHECK_FALSE(diff.valid());
    }

    TEST_CASE("Duration +/- Duration is exact at the finer unit") {
        Series a = make_duration(NANOARROW_TIME_UNIT_SECOND, {2});
        Series b = make_duration(NANOARROW_TIME_UNIT_MILLI, {500});
        Series sum = a.add(b);
        REQUIRE(sum.valid());
        CHECK(sum.type() == TypeId::Duration);
        CHECK(sum.data_type().time_unit ==
              dftracer::utils::dataframe::TimeUnit::Milli);
        CHECK(sum.data<std::int64_t>()[0] == 2500);

        Series diff = a.sub(b);
        REQUIRE(diff.valid());
        CHECK(diff.data<std::int64_t>()[0] == 1500);
    }

    TEST_CASE(
        "Duration * scalar and Duration / scalar stay Duration, "
        "unit unchanged") {
        Series d = make_duration(NANOARROW_TIME_UNIT_MILLI, {100, 200});
        dftu_scalar three{};
        three.kind = DFTU_SCALAR_TAG_I64;
        three.value.i = 3;
        dftu_series* prod_raw = dftu_series_mul_scalar(d.handle(), three);
        REQUIRE(prod_raw != nullptr);
        Series prod{prod_raw};
        CHECK(prod.type() == TypeId::Duration);
        CHECK(prod.data_type().time_unit ==
              dftracer::utils::dataframe::TimeUnit::Milli);
        CHECK(prod.data<std::int64_t>()[0] == 300);
        CHECK(prod.data<std::int64_t>()[1] == 600);

        dftu_series* quot_raw = dftu_series_div_scalar(d.handle(), three);
        REQUIRE(quot_raw != nullptr);
        Series quot{quot_raw};
        CHECK(quot.type() == TypeId::Duration);
        CHECK(quot.data<std::int64_t>()[0] == 33);  // truncating int divide
    }

    TEST_CASE(
        "Timestamp +/- a plain scalar is refused: a scalar has no "
        "unit") {
        Series ts = make_timestamp({100});
        dftu_scalar one{};
        one.kind = DFTU_SCALAR_TAG_I64;
        one.value.i = 1;
        dftu_series* sum_raw = dftu_series_add_scalar(ts.handle(), one);
        CHECK(sum_raw == nullptr);
    }

    TEST_CASE(
        "Duration SUM reduces to a Duration total; PRODUCT stays "
        "refused") {
        Series d = make_duration(NANOARROW_TIME_UNIT_MICRO, {10, 20, 30});
        REQUIRE(d.valid());
        Scalar total = d.sum();
        CHECK(total.i64() == 60);

        dftu_scalar prod = dftu_series_product(d.handle());
        // product is refused for temporal types; the untouched scalar reads
        // as its zero-initialized I64 kind with value 0, not a real product.
        CHECK(prod.kind == DFTU_SCALAR_TAG_I64);
        CHECK(prod.value.i == 0);
    }

    TEST_CASE(
        "Time64 - Time64 is an exact Duration; Time64 + Time64 is "
        "refused") {
        Series a = make_time64(NANOARROW_TIME_UNIT_MICRO, {5000, 9000});
        Series b = make_time64(NANOARROW_TIME_UNIT_MICRO, {1000, 2000});
        Series diff = a.sub(b);
        REQUIRE(diff.valid());
        CHECK(diff.type() == TypeId::Duration);
        CHECK(diff.data<std::int64_t>()[0] == 4000);
        CHECK(diff.data<std::int64_t>()[1] == 7000);

        Series sum = a.add(b);
        CHECK_FALSE(sum.valid());
    }

    TEST_CASE("Time64 +/- Duration is an exact Time64 at the finer unit") {
        Series t = make_time64(NANOARROW_TIME_UNIT_MICRO, {5000});
        Series dur = make_duration(NANOARROW_TIME_UNIT_NANO, {2000});
        Series sum = t.add(dur);
        REQUIRE(sum.valid());
        CHECK(sum.type() == TypeId::Time64);
        CHECK(sum.data_type().time_unit ==
              dftracer::utils::dataframe::TimeUnit::Nano);
        CHECK(sum.data<std::int64_t>()[0] == 5002000);  // 5000us + 2000ns
    }

    TEST_CASE(
        "Time64 + a coarser (Second) Duration still resolves to "
        "Time64's own finer unit") {
        Series t = make_time64(NANOARROW_TIME_UNIT_MICRO, {5000});
        Series dur = make_duration(NANOARROW_TIME_UNIT_SECOND, {1});
        Series sum = t.add(dur);
        REQUIRE(sum.valid());
        CHECK(sum.type() == TypeId::Time64);
        CHECK(sum.data_type().time_unit ==
              dftracer::utils::dataframe::TimeUnit::Micro);
        CHECK(sum.data<std::int64_t>()[0] == 1005000);  // 5000us + 1,000,000us
    }

    TEST_CASE(
        "Date32 arithmetic stays refused: no TimeUnit to reconcile "
        "against a Duration tick") {
        std::vector<std::int32_t> days = {100, 200};
        Series a = Series::flat(TypeId::Date32, days.data(), 2);
        Series b = Series::flat(TypeId::Date32, days.data(), 2);
        REQUIRE(a.valid());
        Series sum = a.add(b);
        CHECK_FALSE(sum.valid());
        Series diff = a.sub(b);
        CHECK_FALSE(diff.valid());
    }

    TEST_CASE(
        "DataFrame::group_by groups a FixedSizeBinary key by exact byte "
        "equality, keeping the width") {
        Series keys = make_fixed_size_binary(
            {"aaaa", "bbbb", "aaaa", "cccc", "bbbb", "aaaa"}, 4);
        std::vector<std::int64_t> ones(6, 1);
        Series values = Series::flat_i64(ones.data(), 6);
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(keys.share());
        df.columns.push_back(values.share());
        DataFrame grouped = df.group_by("k", {GroupAgg{Agg::Count, "v", "n"}});
        REQUIRE(grouped.num_rows() == 3);
        Series gk = grouped.column("k");
        REQUIRE(gk.type() == TypeId::FixedSizeBinary);
        CHECK(gk.data_type().fixed_size == 4);
        const std::int64_t* n = grouped.column("n").data<std::int64_t>();
        REQUIRE(n != nullptr);
        const char* d = gk.data<char>();
        REQUIRE(d != nullptr);
        for (std::int64_t r = 0; r < 3; ++r) {
            const std::string key(d + r * 4, 4);
            if (key == "aaaa")
                CHECK(n[r] == 3);
            else if (key == "bbbb")
                CHECK(n[r] == 2);
            else if (key == "cccc")
                CHECK(n[r] == 1);
            else
                FAIL("unexpected FixedSizeBinary group key: " << key);
        }
    }

    TEST_CASE(
        "FixedSizeBinary group keys merge correctly across two partial "
        "AggStates") {
        Series k1 = make_fixed_size_binary({"aaaa", "bbbb", "aaaa"}, 4);
        Series k2 = make_fixed_size_binary({"cccc", "bbbb", "aaaa"}, 4);
        std::vector<std::int64_t> ones3(3, 1);
        Series v1 = Series::flat_i64(ones3.data(), 3);
        Series v2 = Series::flat_i64(ones3.data(), 3);

        AggStatePtr st1 = df::agg_new({AggSpec{AggOp::Count, -1, "n"}});
        df::agg_accumulate(*st1, std::vector<const Series*>{&k1},
                           std::vector<const Series*>{&v1});
        AggStatePtr st2 = df::agg_new({AggSpec{AggOp::Count, -1, "n"}});
        df::agg_accumulate(*st2, std::vector<const Series*>{&k2},
                           std::vector<const Series*>{&v2});
        df::agg_merge(*st1, *st2);
        REQUIRE(df::agg_num_groups(*st1) == 3);

        DataFrame out = df::agg_finalize(*st1, "k");
        REQUIRE(out.num_rows() == 3);
        CHECK(out.column("k").type() == TypeId::FixedSizeBinary);
        CHECK(out.column("k").data_type().fixed_size == 4);
        const std::int64_t* n = out.column("n").data<std::int64_t>();
        for (std::int64_t g = 0; g < df::agg_num_groups(*st1); ++g) {
            const std::vector<std::string> key = df::agg_group_key(*st1, g);
            REQUIRE(key.size() == 1);
            if (key[0] == "aaaa")
                CHECK(n[g] == 3);
            else if (key[0] == "bbbb")
                CHECK(n[g] == 2);
            else if (key[0] == "cccc")
                CHECK(n[g] == 1);
            else
                FAIL("unexpected FixedSizeBinary group key: " << key[0]);
        }
    }

    TEST_CASE(
        "FixedSizeBinary group state survives a serialize/deserialize "
        "(spill) round trip") {
        Series keys = make_fixed_size_binary(
            {"aaaa", "bbbb", "aaaa", "cccc", "bbbb", "aaaa"}, 4);
        std::vector<std::int64_t> ones(6, 1);
        Series values = Series::flat_i64(ones.data(), 6);

        AggStatePtr st = df::agg_new({AggSpec{AggOp::Count, -1, "n"}});
        df::agg_accumulate(*st, std::vector<const Series*>{&keys},
                           std::vector<const Series*>{&values});
        const std::string blob = df::agg_serialize(*st);
        AggStatePtr back = df::agg_deserialize(blob);

        REQUIRE(df::agg_num_groups(*back) == 3);
        DataFrame out = df::agg_finalize(*back, "k");
        REQUIRE(out.num_rows() == 3);
        CHECK(out.column("k").type() == TypeId::FixedSizeBinary);
        CHECK(out.column("k").data_type().fixed_size == 4);
        const std::int64_t* n = out.column("n").data<std::int64_t>();
        const char* d = out.column("k").data<char>();
        for (std::int64_t r = 0; r < 3; ++r) {
            const std::string key(d + r * 4, 4);
            if (key == "aaaa")
                CHECK(n[r] == 3);
            else if (key == "bbbb")
                CHECK(n[r] == 2);
            else if (key == "cccc")
                CHECK(n[r] == 1);
            else
                FAIL("unexpected FixedSizeBinary group key: " << key);
        }
    }

    TEST_CASE(
        "DataFrame::group_by groups a Decimal128(38, 9) key by exact "
        "value, keeping precision and scale") {
        Series keys = make_decimal128({100, 300, 100, 500, 300, 100}, 38, 9);
        std::vector<std::int64_t> ones(6, 1);
        Series values = Series::flat_i64(ones.data(), 6);
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(keys.share());
        df.columns.push_back(values.share());
        DataFrame grouped = df.group_by("k", {GroupAgg{Agg::Count, "v", "n"}});
        REQUIRE(grouped.num_rows() == 3);
        Series gk = grouped.column("k");
        REQUIRE(gk.type() == TypeId::Decimal128);
        const DataType dt = gk.data_type();
        CHECK(dt.decimal_precision == 38);
        CHECK(dt.decimal_scale == 9);
        const std::int64_t* n = grouped.column("n").data<std::int64_t>();
        REQUIRE(n != nullptr);
        const std::uint8_t* d = gk.data<std::uint8_t>();
        REQUIRE(d != nullptr);
        for (std::int64_t r = 0; r < 3; ++r) {
            std::int64_t v;
            std::memcpy(&v, d + r * 16, 8);
            if (v == 100)
                CHECK(n[r] == 3);
            else if (v == 300)
                CHECK(n[r] == 2);
            else if (v == 500)
                CHECK(n[r] == 1);
            else
                FAIL("unexpected Decimal128 group key: " << v);
        }
    }

    TEST_CASE(
        "Decimal128 group keys merge correctly across two partial "
        "AggStates and survive a spill round trip") {
        Series k1 = make_decimal128({100, 300, 100}, 38, 9);
        Series k2 = make_decimal128({500, 300, 100}, 38, 9);
        std::vector<std::int64_t> ones3(3, 1);
        Series v1 = Series::flat_i64(ones3.data(), 3);
        Series v2 = Series::flat_i64(ones3.data(), 3);

        AggStatePtr st1 = df::agg_new({AggSpec{AggOp::Count, -1, "n"}});
        df::agg_accumulate(*st1, std::vector<const Series*>{&k1},
                           std::vector<const Series*>{&v1});
        AggStatePtr st2 = df::agg_new({AggSpec{AggOp::Count, -1, "n"}});
        df::agg_accumulate(*st2, std::vector<const Series*>{&k2},
                           std::vector<const Series*>{&v2});
        df::agg_merge(*st1, *st2);
        REQUIRE(df::agg_num_groups(*st1) == 3);

        // Round-trip the merged partial through the spill blob before
        // finalizing, exercising the same path agg/serialize.cpp writes.
        const std::string blob = df::agg_serialize(*st1);
        AggStatePtr back = df::agg_deserialize(blob);
        REQUIRE(df::agg_num_groups(*back) == 3);

        DataFrame out = df::agg_finalize(*back, "k");
        REQUIRE(out.num_rows() == 3);
        CHECK(out.column("k").type() == TypeId::Decimal128);
        const DataType dt = out.column("k").data_type();
        CHECK(dt.decimal_precision == 38);
        CHECK(dt.decimal_scale == 9);
        const std::int64_t* n = out.column("n").data<std::int64_t>();
        const std::uint8_t* d = out.column("k").data<std::uint8_t>();
        for (std::int64_t r = 0; r < 3; ++r) {
            std::int64_t v;
            std::memcpy(&v, d + r * 16, 8);
            if (v == 100)
                CHECK(n[r] == 3);
            else if (v == 300)
                CHECK(n[r] == 2);
            else if (v == 500)
                CHECK(n[r] == 1);
            else
                FAIL("unexpected Decimal128 group key: " << v);
        }
    }

    TEST_CASE(
        "DataFrame::group_by groups a Decimal256 key by exact value, "
        "keeping precision and scale") {
        Series keys = make_decimal256({100, 300, 100, 500, 300, 100}, 50, 20);
        std::vector<std::int64_t> ones(6, 1);
        Series values = Series::flat_i64(ones.data(), 6);
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(keys.share());
        df.columns.push_back(values.share());
        DataFrame grouped = df.group_by("k", {GroupAgg{Agg::Count, "v", "n"}});
        REQUIRE(grouped.num_rows() == 3);
        Series gk = grouped.column("k");
        REQUIRE(gk.type() == TypeId::Decimal256);
        const DataType dt = gk.data_type();
        CHECK(dt.decimal_precision == 50);
        CHECK(dt.decimal_scale == 20);
        const std::int64_t* n = grouped.column("n").data<std::int64_t>();
        REQUIRE(n != nullptr);
        const std::uint8_t* d = gk.data<std::uint8_t>();
        REQUIRE(d != nullptr);
        for (std::int64_t r = 0; r < 3; ++r) {
            std::int64_t v;
            std::memcpy(&v, d + r * 32, 8);
            if (v == 100)
                CHECK(n[r] == 3);
            else if (v == 300)
                CHECK(n[r] == 2);
            else if (v == 500)
                CHECK(n[r] == 1);
            else
                FAIL("unexpected Decimal256 group key: " << v);
        }
    }

    TEST_CASE(
        "Decimal256 group keys merge correctly across two partial "
        "AggStates and survive a spill round trip") {
        Series k1 = make_decimal256({100, 300, 100}, 50, 20);
        Series k2 = make_decimal256({500, 300, 100}, 50, 20);
        std::vector<std::int64_t> ones3(3, 1);
        Series v1 = Series::flat_i64(ones3.data(), 3);
        Series v2 = Series::flat_i64(ones3.data(), 3);

        AggStatePtr st1 = df::agg_new({AggSpec{AggOp::Count, -1, "n"}});
        df::agg_accumulate(*st1, std::vector<const Series*>{&k1},
                           std::vector<const Series*>{&v1});
        AggStatePtr st2 = df::agg_new({AggSpec{AggOp::Count, -1, "n"}});
        df::agg_accumulate(*st2, std::vector<const Series*>{&k2},
                           std::vector<const Series*>{&v2});
        df::agg_merge(*st1, *st2);
        REQUIRE(df::agg_num_groups(*st1) == 3);

        const std::string blob = df::agg_serialize(*st1);
        AggStatePtr back = df::agg_deserialize(blob);
        REQUIRE(df::agg_num_groups(*back) == 3);

        DataFrame out = df::agg_finalize(*back, "k");
        REQUIRE(out.num_rows() == 3);
        CHECK(out.column("k").type() == TypeId::Decimal256);
        const DataType dt = out.column("k").data_type();
        CHECK(dt.decimal_precision == 50);
        CHECK(dt.decimal_scale == 20);
        const std::int64_t* n = out.column("n").data<std::int64_t>();
        const std::uint8_t* d = out.column("k").data<std::uint8_t>();
        for (std::int64_t r = 0; r < 3; ++r) {
            std::int64_t v;
            std::memcpy(&v, d + r * 32, 8);
            if (v == 100)
                CHECK(n[r] == 3);
            else if (v == 300)
                CHECK(n[r] == 2);
            else if (v == 500)
                CHECK(n[r] == 1);
            else
                FAIL("unexpected Decimal256 group key: " << v);
        }
    }

    TEST_CASE(
        "DataFrame::group_by groups a Binary key by exact byte equality") {
        Series keys =
            make_binary({"aaaa", "bbbb", "aaaa", "cccc", "bbbb", "aaaa"});
        std::vector<std::int64_t> ones(6, 1);
        Series values = Series::flat_i64(ones.data(), 6);
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(keys.share());
        df.columns.push_back(values.share());
        DataFrame grouped = df.group_by("k", {GroupAgg{Agg::Count, "v", "n"}});
        REQUIRE(grouped.num_rows() == 3);
        Series gk = grouped.column("k");
        REQUIRE(gk.type() == TypeId::Binary);
        const std::int64_t* n = grouped.column("n").data<std::int64_t>();
        REQUIRE(n != nullptr);
        for (std::int64_t r = 0; r < 3; ++r) {
            const std::string key(gk.string_at(r));
            if (key == "aaaa")
                CHECK(n[r] == 3);
            else if (key == "bbbb")
                CHECK(n[r] == 2);
            else if (key == "cccc")
                CHECK(n[r] == 1);
            else
                FAIL("unexpected Binary group key: " << key);
        }
    }

    TEST_CASE(
        "Binary group keys merge correctly across two partial AggStates and "
        "survive a spill round trip") {
        Series k1 = make_binary({"aaaa", "bbbb", "aaaa"});
        Series k2 = make_binary({"cccc", "bbbb", "aaaa"});
        std::vector<std::int64_t> ones3(3, 1);
        Series v1 = Series::flat_i64(ones3.data(), 3);
        Series v2 = Series::flat_i64(ones3.data(), 3);

        AggStatePtr st1 = df::agg_new({AggSpec{AggOp::Count, -1, "n"}});
        df::agg_accumulate(*st1, std::vector<const Series*>{&k1},
                           std::vector<const Series*>{&v1});
        AggStatePtr st2 = df::agg_new({AggSpec{AggOp::Count, -1, "n"}});
        df::agg_accumulate(*st2, std::vector<const Series*>{&k2},
                           std::vector<const Series*>{&v2});
        df::agg_merge(*st1, *st2);
        REQUIRE(df::agg_num_groups(*st1) == 3);

        const std::string blob = df::agg_serialize(*st1);
        AggStatePtr back = df::agg_deserialize(blob);
        REQUIRE(df::agg_num_groups(*back) == 3);

        DataFrame out = df::agg_finalize(*back, "k");
        REQUIRE(out.num_rows() == 3);
        CHECK(out.column("k").type() == TypeId::Binary);
        const std::int64_t* n = out.column("n").data<std::int64_t>();
        Series gk = out.column("k");
        for (std::int64_t r = 0; r < 3; ++r) {
            const std::string key(gk.string_at(r));
            if (key == "aaaa")
                CHECK(n[r] == 3);
            else if (key == "bbbb")
                CHECK(n[r] == 2);
            else if (key == "cccc")
                CHECK(n[r] == 1);
            else
                FAIL("unexpected Binary group key: " << key);
        }
    }

    TEST_CASE(
        "DataFrame::group_by groups a LargeBinary key by exact byte "
        "equality") {
        Series keys =
            make_large_binary({"aaaa", "bbbb", "aaaa", "cccc", "bbbb", "aaaa"});
        std::vector<std::int64_t> ones(6, 1);
        Series values = Series::flat_i64(ones.data(), 6);
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(keys.share());
        df.columns.push_back(values.share());
        DataFrame grouped = df.group_by("k", {GroupAgg{Agg::Count, "v", "n"}});
        REQUIRE(grouped.num_rows() == 3);
        Series gk = grouped.column("k");
        REQUIRE(gk.type() == TypeId::LargeBinary);
        const std::int64_t* n = grouped.column("n").data<std::int64_t>();
        REQUIRE(n != nullptr);
        for (std::int64_t r = 0; r < 3; ++r) {
            const std::string key(gk.string_at(r));
            if (key == "aaaa")
                CHECK(n[r] == 3);
            else if (key == "bbbb")
                CHECK(n[r] == 2);
            else if (key == "cccc")
                CHECK(n[r] == 1);
            else
                FAIL("unexpected LargeBinary group key: " << key);
        }
    }

    TEST_CASE(
        "LargeBinary group keys merge correctly across two partial "
        "AggStates and survive a spill round trip") {
        Series k1 = make_large_binary({"aaaa", "bbbb", "aaaa"});
        Series k2 = make_large_binary({"cccc", "bbbb", "aaaa"});
        std::vector<std::int64_t> ones3(3, 1);
        Series v1 = Series::flat_i64(ones3.data(), 3);
        Series v2 = Series::flat_i64(ones3.data(), 3);

        AggStatePtr st1 = df::agg_new({AggSpec{AggOp::Count, -1, "n"}});
        df::agg_accumulate(*st1, std::vector<const Series*>{&k1},
                           std::vector<const Series*>{&v1});
        AggStatePtr st2 = df::agg_new({AggSpec{AggOp::Count, -1, "n"}});
        df::agg_accumulate(*st2, std::vector<const Series*>{&k2},
                           std::vector<const Series*>{&v2});
        df::agg_merge(*st1, *st2);
        REQUIRE(df::agg_num_groups(*st1) == 3);

        const std::string blob = df::agg_serialize(*st1);
        AggStatePtr back = df::agg_deserialize(blob);
        REQUIRE(df::agg_num_groups(*back) == 3);

        DataFrame out = df::agg_finalize(*back, "k");
        REQUIRE(out.num_rows() == 3);
        CHECK(out.column("k").type() == TypeId::LargeBinary);
        const std::int64_t* n = out.column("n").data<std::int64_t>();
        Series gk = out.column("k");
        for (std::int64_t r = 0; r < 3; ++r) {
            const std::string key(gk.string_at(r));
            if (key == "aaaa")
                CHECK(n[r] == 3);
            else if (key == "bbbb")
                CHECK(n[r] == 2);
            else if (key == "cccc")
                CHECK(n[r] == 1);
            else
                FAIL("unexpected LargeBinary group key: " << key);
        }
    }

    TEST_CASE(
        "DataFrame::group_by groups a Float16 key by exact bit pattern, "
        "keeping +-0.0 and NaN distinct") {
        // rows: 1.0, -0.0, 1.0, +0.0, NaN, -0.0 -> four groups: {1.0}x2,
        // {-0.0}x2, {+0.0}x1, {NaN}x1. -0.0/+0.0 must stay separate groups
        // (their half bit patterns differ) and NaN must group with itself
        // (bit-pattern equality, not IEEE float equality where NaN != NaN).
        Series keys =
            make_float16({1.0f, -0.0f, 1.0f, 0.0f, std::nanf(""), -0.0f});
        std::vector<std::int64_t> ones(6, 1);
        Series values = Series::flat_i64(ones.data(), 6);
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(keys.share());
        df.columns.push_back(values.share());
        DataFrame grouped = df.group_by("k", {GroupAgg{Agg::Count, "v", "n"}});
        REQUIRE(grouped.num_rows() == 4);
        Series gk = grouped.column("k");
        REQUIRE(gk.type() == TypeId::Float16);
        const std::uint16_t* bits = gk.data<std::uint16_t>();
        REQUIRE(bits != nullptr);
        const std::int64_t* n = grouped.column("n").data<std::int64_t>();
        REQUIRE(n != nullptr);
        int seen_one = 0, seen_neg_zero = 0, seen_pos_zero = 0, seen_nan = 0;
        for (std::int64_t r = 0; r < 4; ++r) {
            const float f = df::half_to_float(bits[r]);
            if (std::isnan(f)) {
                ++seen_nan;
                CHECK(n[r] == 1);
            } else if (f == 0.0f) {
                if (std::signbit(f)) {
                    ++seen_neg_zero;
                    CHECK(n[r] == 2);
                } else {
                    ++seen_pos_zero;
                    CHECK(n[r] == 1);
                }
            } else if (f == 1.0f) {
                ++seen_one;
                CHECK(n[r] == 2);
            } else {
                FAIL("unexpected Float16 group key bits: " << bits[r]);
            }
        }
        CHECK(seen_one == 1);
        CHECK(seen_neg_zero == 1);
        CHECK(seen_pos_zero == 1);
        CHECK(seen_nan == 1);
    }

    TEST_CASE(
        "Float16 group keys merge correctly across two partial AggStates "
        "and survive a spill round trip") {
        Series k1 = make_float16({1.0f, -0.0f, std::nanf("")});
        Series k2 = make_float16({0.0f, -0.0f, 1.0f});
        std::vector<std::int64_t> ones3(3, 1);
        Series v1 = Series::flat_i64(ones3.data(), 3);
        Series v2 = Series::flat_i64(ones3.data(), 3);

        AggStatePtr st1 = df::agg_new({AggSpec{AggOp::Count, -1, "n"}});
        df::agg_accumulate(*st1, std::vector<const Series*>{&k1},
                           std::vector<const Series*>{&v1});
        AggStatePtr st2 = df::agg_new({AggSpec{AggOp::Count, -1, "n"}});
        df::agg_accumulate(*st2, std::vector<const Series*>{&k2},
                           std::vector<const Series*>{&v2});
        df::agg_merge(*st1, *st2);
        // {1.0}x2, {-0.0}x2, {+0.0}x1, {NaN}x1
        REQUIRE(df::agg_num_groups(*st1) == 4);

        const std::string blob = df::agg_serialize(*st1);
        AggStatePtr back = df::agg_deserialize(blob);
        REQUIRE(df::agg_num_groups(*back) == 4);

        DataFrame out = df::agg_finalize(*back, "k");
        REQUIRE(out.num_rows() == 4);
        CHECK(out.column("k").type() == TypeId::Float16);
        const std::uint16_t* bits = out.column("k").data<std::uint16_t>();
        REQUIRE(bits != nullptr);
        const std::int64_t* n = out.column("n").data<std::int64_t>();
        int seen_one = 0, seen_neg_zero = 0, seen_pos_zero = 0, seen_nan = 0;
        for (std::int64_t r = 0; r < 4; ++r) {
            const float f = df::half_to_float(bits[r]);
            if (std::isnan(f)) {
                ++seen_nan;
                CHECK(n[r] == 1);
            } else if (f == 0.0f) {
                if (std::signbit(f)) {
                    ++seen_neg_zero;
                    CHECK(n[r] == 2);
                } else {
                    ++seen_pos_zero;
                    CHECK(n[r] == 1);
                }
            } else if (f == 1.0f) {
                ++seen_one;
                CHECK(n[r] == 2);
            } else {
                FAIL("unexpected Float16 group key bits: " << bits[r]);
            }
        }
        CHECK(seen_one == 1);
        CHECK(seen_neg_zero == 1);
        CHECK(seen_pos_zero == 1);
        CHECK(seen_nan == 1);
    }
}
#else
TEST_SUITE("dataframe_arrow_types_compute") {
    TEST_CASE("Arrow disabled: nothing to exercise") { CHECK(true); }
}
#endif
