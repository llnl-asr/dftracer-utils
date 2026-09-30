#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

using dftracer::utils::dataframe::Series;

namespace {

const std::vector<double> BINS = {0, 80, 100, 120, 1e9};

Series f64(const std::vector<double>& v,
           const std::vector<std::uint8_t>& valid = {}) {
    return Series::flat_f64(v.data(), static_cast<std::int64_t>(v.size()),
                            valid.empty() ? nullptr : valid.data());
}

std::vector<std::int32_t> ints(const Series& s) {
    return std::vector<std::int32_t>(s.data<std::int32_t>(),
                                     s.data<std::int32_t>() + s.length());
}

}  // namespace

TEST_CASE("the default is unchanged") {
    Series r = f64({80, 100, 120, 95}).cut(f64(BINS));
    REQUIRE(r.valid());
    CHECK(ints(r) == std::vector<std::int32_t>{2, 3, 4, 2});
}

TEST_CASE("right-closed interior bins equal pandas cut(labels=False)") {
    Series r =
        f64({80, 100, 120, 95}).cut(f64(BINS), DFTU_CUT_RIGHT | DFTU_CUT_INNER);
    REQUIRE(r.valid());
    CHECK(ints(r) == std::vector<std::int32_t>{0, 1, 2, 1});
}

TEST_CASE("values outside the range are null with inner bins") {
    Series r = f64({0, 2e9}).cut(f64(BINS), DFTU_CUT_RIGHT | DFTU_CUT_INNER);
    REQUIRE(r.valid());
    CHECK(r.is_null(0));
    CHECK(r.is_null(1));
}

TEST_CASE("right-closed with outer bins") {
    Series r = f64({80, 0, 2e9}).cut(f64(BINS), DFTU_CUT_RIGHT);
    REQUIRE(r.valid());
    CHECK(ints(r) == std::vector<std::int32_t>{1, 0, 5});
}

TEST_CASE("a null value gives a null bin and an unknown flag is refused") {
    Series r = f64({80, 0.0, 95}, {0x05}).cut(f64(BINS), DFTU_CUT_RIGHT);
    REQUIRE(r.valid());
    CHECK(r.data<std::int32_t>()[0] == 1);
    CHECK(r.is_null(1));
    CHECK_FALSE(f64({1.0}).cut(f64(BINS), 4).valid());
}
