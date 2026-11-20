// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/kernels/group_by.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <span_list.h>
#include <task_result.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

double nan_with(std::uint64_t payload, bool negative) {
    return std::bit_cast<double>((negative ? std::uint64_t{1} << 63 : 0) |
                                 0x7FF0000000000000ULL | payload);
}

float nan32_with(std::uint32_t payload, bool negative) {
    return std::bit_cast<float>((negative ? std::uint32_t{1} << 31 : 0) |
                                0x7F800000U | payload);
}

// Rows: -0.0, 0.0, 1.5, three NaNs of other sign and payload, 0.0, 1.5.
dstd::Vector<double> f64_rows() {
    return {-0.0,
            0.0,
            1.5,
            std::numeric_limits<double>::quiet_NaN(),
            nan_with(0x123, true),
            nan_with(0x7, false),
            0.0,
            1.5};
}

DataFrame count_by(const Series& key) {
    dstd::Vector<std::int64_t> ones(static_cast<std::size_t>(key.length()), 1);
    Series v = Series::flat_i64(ones.data(), key.length());
    dstd::Vector<const Series*> keys{&key};
    dstd::Vector<const Series*> values{&v};
    return dftu_utils_test::ok(
        dataframe::group_agg(keys, values, {AggSpec{AggOp::Count, -1, "n"}},
                             dstd::Vector<dstd::String>{"k"}));
}

// The count of the group whose key is `pred`, or -1 when there is none.
template <class T, class Pred>
std::int64_t count_where(const DataFrame& r, Pred pred) {
    const T* k = r.column("k").data<T>();
    const std::int64_t* n = r.column("n").data<std::int64_t>();
    for (std::int64_t i = 0; i < r.num_rows(); ++i)
        if (pred(k[i])) return n[i];
    return -1;
}

}  // namespace

TEST_CASE("group_agg keys -0.0 as 0.0 and every NaN as one value") {
    dstd::Vector<double> k = f64_rows();
    Series sk = Series::flat_f64(k.data(), static_cast<std::int64_t>(k.size()));
    const DataFrame r = count_by(sk);
    CHECK(r.num_rows() == 3);
    CHECK(count_where<double>(r, [](double x) { return x == 0.0; }) == 3);
    CHECK(count_where<double>(r, [](double x) { return x == 1.5; }) == 2);
    CHECK(count_where<double>(r, [](double x) { return std::isnan(x); }) == 3);
}

TEST_CASE("group_agg keys a Float32 as 0.0 and every NaN as one value") {
    dstd::Vector<float> k{-0.0f,
                          0.0f,
                          1.5f,
                          std::numeric_limits<float>::quiet_NaN(),
                          nan32_with(0x123, true),
                          nan32_with(0x7, false),
                          0.0f,
                          1.5f};
    Series sk = Series::flat(TypeId::Float32, k.data(),
                             static_cast<std::int64_t>(k.size()));
    const DataFrame r = count_by(sk);
    CHECK(r.num_rows() == 3);
    // The group-by writes a float key as Float64.
    REQUIRE(r.column("k").type() == TypeId::Float64);
    CHECK(count_where<double>(r, [](double x) { return x == 0.0; }) == 3);
    CHECK(count_where<double>(r, [](double x) { return std::isnan(x); }) == 3);
}

TEST_CASE(
    "group_agg on two float keys and a large frame follows the same rule") {
    constexpr std::int64_t N = 400000;
    dstd::Vector<double> a, b;
    a.reserve(N);
    b.reserve(N);
    const dstd::Vector<double> pool = f64_rows();
    for (std::int64_t i = 0; i < N; ++i) {
        a.push_back(pool[static_cast<std::size_t>(i % 8)]);
        b.push_back(i % 2 == 0 ? -0.0 : 0.0);
    }
    Series sa = Series::flat_f64(a.data(), N);
    Series sb = Series::flat_f64(b.data(), N);
    dstd::Vector<std::int64_t> ones(static_cast<std::size_t>(N), 1);
    Series v = Series::flat_i64(ones.data(), N);
    dstd::Vector<const Series*> keys{&sa, &sb};
    dstd::Vector<const Series*> values{&v};
    const DataFrame r = dftu_utils_test::ok(
        dataframe::group_agg(keys, values, {AggSpec{AggOp::Count, -1, "n"}},
                             dstd::Vector<dstd::String>{"k", "j"}));
    CHECK(r.num_rows() == 3);
    const std::int64_t* n = r.column("n").data<std::int64_t>();
    std::int64_t total = 0;
    for (std::int64_t i = 0; i < r.num_rows(); ++i) total += n[i];
    CHECK(total == N);
}

TEST_CASE("value_counts counts -0.0 with 0.0 and every NaN together") {
    dstd::Vector<double> k = f64_rows();
    Series sk = Series::flat_f64(k.data(), static_cast<std::int64_t>(k.size()));
    const DataFrame r = dftu_utils_test::ok(sk.value_counts());
    REQUIRE(r.num_rows() == 3);
    const std::int64_t* n = r.column("count").data<std::int64_t>();
    CHECK(n[0] == 3);
    CHECK(n[1] == 3);
    CHECK(n[2] == 2);
}

TEST_CASE(
    "value_counts of a Float32 counts -0.0 with 0.0 and every NaN together") {
    dstd::Vector<float> k{-0.0f, 0.0f, nan32_with(0x5, true),
                          std::numeric_limits<float>::quiet_NaN()};
    Series sk = Series::flat(TypeId::Float32, k.data(), 4);
    const DataFrame r = dftu_utils_test::ok(sk.value_counts());
    CHECK(r.num_rows() == 2);
}

namespace {

// -0.0 and 0.0, then NaNs of other sign and payload (quiet and signaling),
// then 1.0 twice, as half-float bit patterns.
dstd::Vector<std::uint16_t> f16_bits() {
    return {0x8000, 0x0000, 0xFE00, 0x7E01, 0x7C01, 0xFC01, 0x3C00, 0x3C00};
}

dstd::Vector<std::int64_t> sorted_counts(const DataFrame& r,
                                         dstd::StringView column) {
    const std::int64_t* n = r.column(column).data<std::int64_t>();
    dstd::Vector<std::int64_t> out(n, n + r.num_rows());
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("Float16 keys: -0.0, 0.0 and every NaN bit pattern are one value") {
    dstd::Vector<std::uint16_t> bits = f16_bits();
    Series sk = Series::flat(TypeId::Float16, bits.data(),
                             static_cast<std::int64_t>(bits.size()));
    const DataFrame grouped = count_by(sk);
    CHECK(sorted_counts(grouped, "n") == dstd::Vector<std::int64_t>{2, 2, 4});
    const DataFrame counted = dftu_utils_test::ok(sk.value_counts());
    CHECK(sorted_counts(counted, "count") ==
          dstd::Vector<std::int64_t>{2, 2, 4});
    CHECK(sk.unique().length() == 3);
    CHECK(sk.nunique() == 3);
}

TEST_CASE("unique and nunique key a float as pandas and Polars do") {
    dstd::Vector<double> k = f64_rows();
    Series sk = Series::flat_f64(k.data(), static_cast<std::int64_t>(k.size()));
    CHECK(sk.unique().length() == 3);
    CHECK(sk.nunique() == 3);
    CHECK(sk.unique(true).length() == 3);
}

TEST_CASE("duplicated and a frame unique key a float as pandas does") {
    dstd::Vector<double> k = f64_rows();
    Series sk = Series::flat_f64(k.data(), static_cast<std::int64_t>(k.size()));
    const Series dup = sk.duplicated();
    // rows: -0.0, 0.0*, 1.5, NaN, NaN*, NaN*, 0.0*, 1.5*
    const std::uint8_t* flags = dup.data<std::uint8_t>();
    CHECK(((flags[0] >> 0) & 1) == 0);
    CHECK(((flags[0] >> 1) & 1) == 1);
    CHECK(((flags[0] >> 3) & 1) == 0);
    CHECK(((flags[0] >> 4) & 1) == 1);
    CHECK(((flags[0] >> 5) & 1) == 1);
}

TEST_CASE("a join matches -0.0 with 0.0 and a NaN with a NaN") {
    dstd::Vector<double> l{0.0, std::numeric_limits<double>::quiet_NaN(), 1.0};
    dstd::Vector<double> r{-0.0, nan_with(0x9, true), 1.0, 7.0};
    dstd::Vector<std::int64_t> x{1, 2, 3};
    dstd::Vector<std::int64_t> y{10, 20, 30, 40};
    DataFrame left, right;
    left.names = dstd::make_shared_strings({"k", "x"});
    left.columns.push_back(Series::flat_f64(l.data(), 3));
    left.columns.push_back(Series::flat_i64(x.data(), 3));
    right.names = dstd::make_shared_strings({"k", "y"});
    right.columns.push_back(Series::flat_f64(r.data(), 4));
    right.columns.push_back(Series::flat_i64(y.data(), 4));
    const DataFrame out = dftu_utils_test::ok(
        left.join(right, dftu_utils_test::span_of<dstd::String>({"k"}),
                  JoinHow::Inner, "_right", false));
    CHECK(out.num_rows() == 3);
}

TEST_CASE("the series group_by kernel keys a float as one value per rule") {
    dstd::Vector<double> k = f64_rows();
    dstd::Vector<std::int64_t> ones(k.size(), 1);
    Series keys =
        Series::flat_f64(k.data(), static_cast<std::int64_t>(k.size()));
    Series values =
        Series::flat_i64(ones.data(), static_cast<std::int64_t>(ones.size()));
    const DataFrame g = group_by(keys, values, DFTU_REDUCE_COUNT);
    CHECK(g.num_rows() == 3);
}

TEST_CASE("is_in matches a NaN needle with a NaN row and -0.0 with 0.0") {
    dstd::Vector<double> v = f64_rows();
    dstd::Vector<double> needles{std::numeric_limits<double>::quiet_NaN(),
                                 -0.0};
    Series sv = Series::flat_f64(v.data(), static_cast<std::int64_t>(v.size()));
    Series sn = Series::flat_f64(needles.data(), 2);
    const Series hit = sv.is_in(sn);
    const std::uint8_t* b = hit.data<std::uint8_t>();
    // rows: -0.0 0.0 1.5 NaN NaN NaN 0.0 1.5
    CHECK(b[0] == 0b01111011);
}

TEST_CASE("mode of a float is 0.0 whatever the zero's sign, and skips NaN") {
    dstd::Vector<double> k{-0.0,
                           0.0,
                           std::numeric_limits<double>::quiet_NaN(),
                           nan_with(0x5, true),
                           std::numeric_limits<double>::quiet_NaN(),
                           2.0};
    Series sk = Series::flat_f64(k.data(), static_cast<std::int64_t>(k.size()));
    const double m = scalar_value<double>(sk.mode());
    CHECK(m == 0.0);
    CHECK_FALSE(std::signbit(m));
}

namespace {

DataFrame float_keyed(dstd::StringView value_name, dstd::Vector<double> keys,
                      dstd::Vector<std::int64_t> values) {
    DataFrame f;
    f.names = dstd::make_shared_strings({"k", dstd::String(value_name)});
    f.columns.push_back(
        Series::flat_f64(keys.data(), static_cast<std::int64_t>(keys.size())));
    f.columns.push_back(Series::flat_i64(
        values.data(), static_cast<std::int64_t>(values.size())));
    return f;
}

}  // namespace

TEST_CASE("a lookup join matches -0.0 with 0.0 and a NaN with a NaN") {
    const DataFrame left = float_keyed(
        "x", {0.0, std::numeric_limits<double>::quiet_NaN(), 1.0, 5.0},
        {1, 2, 3, 4});
    const DataFrame right =
        float_keyed("y", {-0.0, nan_with(0x9, true), 1.0}, {10, 20, 30});
    const DataFrame out = dftu_utils_test::ok(
        left.join(right, dftu_utils_test::span_of<dstd::String>({"k"}),
                  JoinHow::Lookup, "_right", false));
    REQUIRE(out.num_rows() == 4);
    const Series y = out.column("y");
    CHECK(y.null_count() == 1);
    CHECK(y.is_null(3));
    CHECK(y.values<std::int64_t>()[0] == 10);
    CHECK(y.values<std::int64_t>()[1] == 20);
    CHECK(y.values<std::int64_t>()[2] == 30);
}

TEST_CASE("a nest join groups -0.0 with 0.0 and a NaN with a NaN") {
    const DataFrame left = float_keyed(
        "x", {0.0, std::numeric_limits<double>::quiet_NaN(), 1.0}, {1, 2, 3});
    const DataFrame right = float_keyed(
        "y", {-0.0, 0.0, nan_with(0x9, true), nan_with(0x3, false), 1.0},
        {10, 11, 20, 21, 30});
    const DataFrame out = dftu_utils_test::ok(
        left.join(right, dftu_utils_test::span_of<dstd::String>({"k"}),
                  JoinHow::Nest, "_right", false));
    CHECK(out.num_rows() == 3);
    const Series m = out.column("_right").materialize();
    // The matches per left row: 0.0 has two, the NaN has two, 1.0 has one.
    CHECK(m.offsets()[1] - m.offsets()[0] == 2);
    CHECK(m.offsets()[2] - m.offsets()[1] == 2);
    CHECK(m.offsets()[3] - m.offsets()[2] == 1);
}
