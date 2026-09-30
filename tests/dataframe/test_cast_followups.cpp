#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;

namespace {

bool bit(const Series& s, std::int64_t i) {
    return (s.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1;
}

template <class T>
void check_to_bool(const std::vector<T>& x, TypeId type,
                   const std::vector<std::uint8_t>& valid = {}) {
    const auto n = static_cast<std::int64_t>(x.size());
    Series s =
        Series::flat(type, x.data(), n, valid.empty() ? nullptr : valid.data());
    Series r = s.cast(TypeId::Bool);
    REQUIRE(r.valid());
    REQUIRE(r.length() == n);
    for (std::int64_t i = 0; i < n; ++i) {
        const T v = x[static_cast<std::size_t>(i)];
        bool is_null = !valid.empty() && !((valid[i >> 3] >> (i & 7)) & 1);
        if constexpr (std::is_floating_point_v<T>)
            is_null = is_null || std::isnan(v);
        if (is_null) {
            CHECK(r.is_null(i));
        } else {
            CHECK_FALSE(r.is_null(i));
            CHECK(bit(r, i) == (v != T{}));
        }
    }
}

}  // namespace

TEST_CASE("SIMD and scalar number-to-bool agree on every width and length") {
    std::mt19937_64 gen(12345);
    for (std::size_t n : {0, 1, 7, 8, 63, 64, 65, 127, 128, 129, 1000, 4097}) {
        std::vector<std::int32_t> a(n);
        std::vector<std::int64_t> b(n);
        std::vector<float> c(n);
        std::vector<double> d(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto r = gen() % 6;
            a[i] = r < 2 ? 0 : static_cast<std::int32_t>(gen());
            b[i] = r < 2 ? 0 : static_cast<std::int64_t>(gen());
            c[i] = r == 0   ? std::nanf("")
                   : r == 1 ? -0.0f
                   : r == 2 ? INFINITY
                            : static_cast<float>(gen() % 5) - 2.0f;
            d[i] = r == 0   ? std::nan("")
                   : r == 1 ? -0.0
                   : r == 2 ? -HUGE_VAL
                            : static_cast<double>(gen() % 5) - 2.0;
        }
        check_to_bool(a, TypeId::Int32);
        check_to_bool(b, TypeId::Int64);
        check_to_bool(c, TypeId::Float32);
        check_to_bool(d, TypeId::Float64);
        // with input nulls the scalar path runs; it must give the same answers
        std::vector<std::uint8_t> valid((n + 7) / 8, 0xA5);
        check_to_bool(a, TypeId::Int32, valid);
        check_to_bool(d, TypeId::Float64, valid);
    }
}

TEST_CASE("a column without NaN or nulls has no validity after the cast") {
    std::vector<std::int64_t> x{1, 0, 3};
    Series r = Series::flat_i64(x.data(), 3).cast(TypeId::Bool);
    REQUIRE(r.valid());
    CHECK(r.null_count() == 0);
}

TEST_CASE("NaN and infinity cast to fixed text") {
    std::vector<double> d{1.5, std::nan(""), HUGE_VAL, -HUGE_VAL};
    Series t = Series::flat_f64(d.data(), 4).cast(TypeId::String);
    REQUIRE(t.valid());
    CHECK(t.string_at(0) == "1.5");
    CHECK(t.string_at(1) == "nan");
    CHECK(t.string_at(2) == "inf");
    CHECK(t.string_at(3) == "-inf");
    std::vector<float> f{std::nanf(""), INFINITY};
    Series tf = Series::flat(TypeId::Float32, f.data(), 2).cast(TypeId::String);
    REQUIRE(tf.valid());
    CHECK(tf.string_at(0) == "nan");
    CHECK(tf.string_at(1) == "inf");
}
