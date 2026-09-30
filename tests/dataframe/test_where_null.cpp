#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;

namespace {

// The mask `true, false, true, true` as a bit-packed Bool column.
Series mask(std::uint8_t bits, std::int64_t n) {
    return Series::flat(TypeId::Bool, &bits, n);
}

}  // namespace

TEST_CASE("where with an all-null arm gives null where the mask is false") {
    std::vector<double> x{1.0, 2.0, 3.0, 5.0};
    Series s = Series::flat_f64(x.data(), 4);
    Series r = s.where(mask(0b1101, 4), Series::nulls(TypeId::Float64, 4));
    REQUIRE(r.valid());
    CHECK(r.type() == TypeId::Float64);
    CHECK_FALSE(r.is_null(0));
    CHECK(r.is_null(1));
    CHECK(r.data<double>()[2] == 3.0);
    CHECK(r.data<double>()[3] == 5.0);
}

TEST_CASE("where with a null arm keeps an integer column integer") {
    std::vector<std::int64_t> x{1, 2, 3};
    Series r = Series::flat_i64(x.data(), 3)
                   .where(mask(0b101, 3), Series::nulls(TypeId::Int64, 3));
    REQUIRE(r.valid());
    CHECK(r.type() == TypeId::Int64);
    CHECK(r.is_null(1));
    CHECK(r.data<std::int64_t>()[2] == 3);
}

TEST_CASE("where with a null arm: a null in the source stays null") {
    std::vector<double> x{1.0, 0.0, 3.0};
    std::vector<std::uint8_t> valid{0x05};  // row 1 is null
    Series s = Series::flat_f64(x.data(), 3, valid.data());
    Series r = s.where(mask(0b111, 3), Series::nulls(TypeId::Float64, 3));
    REQUIRE(r.valid());
    CHECK(r.is_null(1));
    CHECK(r.data<double>()[0] == 1.0);
}

TEST_CASE("clip bounds at a dtype's limits leave the other side alone") {
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> x{-2.5, 0.0, 3.5, 0.0};
    std::vector<std::uint8_t> valid{0x07};  // row 3 is null
    Series f = Series::flat_f64(x.data(), 4, valid.data());
    Series lower = f.clip(0.0, inf);        // clip(lower=0)
    REQUIRE(lower.valid());
    CHECK(lower.data<double>()[0] == 0.0);
    CHECK(lower.data<double>()[2] == 3.5);
    CHECK(lower.is_null(3));
    Series upper = f.clip(-inf, 1.0);  // clip(upper=1)
    CHECK(upper.data<double>()[0] == -2.5);
    CHECK(upper.data<double>()[2] == 1.0);

    std::vector<std::int32_t> i{-5, 0, 7};
    Series si = Series::flat(TypeId::Int32, i.data(), 3);
    Series ci = si.clip(std::int64_t{0},
                        std::int64_t{std::numeric_limits<std::int32_t>::max()});
    REQUIRE(ci.valid());
    CHECK(ci.type() == TypeId::Int32);
    CHECK(ci.data<std::int32_t>()[0] == 0);
    CHECK(ci.data<std::int32_t>()[2] == 7);
}
