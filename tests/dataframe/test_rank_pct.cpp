#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

using dftracer::utils::dataframe::RankMethod;
using dftracer::utils::dataframe::Series;

namespace {

Series ints(const std::vector<std::int64_t>& v,
            const std::vector<std::uint8_t>& valid = {}) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()),
                            valid.empty() ? nullptr : valid.data());
}

}  // namespace

TEST_CASE("percentile rank divides the average rank by the count") {
    Series r = ints({10, 30, 20}).rank(RankMethod::Average, false, true);
    REQUIRE(r.valid());
    CHECK(r.data<double>()[0] == doctest::Approx(1.0 / 3.0));
    CHECK(r.data<double>()[1] == doctest::Approx(1.0));
    CHECK(r.data<double>()[2] == doctest::Approx(2.0 / 3.0));
}

TEST_CASE("nulls get a null rank and are not counted") {
    Series r = ints({10, 0, 20}, {0x05}).rank(RankMethod::Average, false, true);
    REQUIRE(r.valid());
    CHECK(r.data<double>()[0] == doctest::Approx(0.5));
    CHECK(r.is_null(1));
    CHECK(r.data<double>()[2] == doctest::Approx(1.0));
}

TEST_CASE("dense percentile rank divides by the distinct count") {
    // 10, 30, 20, 20, null, 10 -> dense ranks 1, 3, 2, 2, null, 1 of 3 values
    Series s = ints({10, 30, 20, 20, 0, 10}, {0x2f});
    Series r = s.rank(RankMethod::Dense, false, true);
    REQUIRE(r.valid());
    CHECK(r.data<double>()[0] == doctest::Approx(1.0 / 3.0));
    CHECK(r.data<double>()[1] == doctest::Approx(1.0));
    CHECK(r.data<double>()[2] == doctest::Approx(2.0 / 3.0));
    CHECK(r.is_null(4));
}

TEST_CASE("the default rank is unchanged") {
    Series r = ints({10, 30, 20}).rank(RankMethod::Average);
    CHECK(r.data<double>()[0] == 1.0);
    CHECK(r.data<double>()[1] == 3.0);
    CHECK(r.data<double>()[2] == 2.0);
}

TEST_CASE("the C ABI flags: 1 is still descending, 2 is percentile") {
    Series s = ints({10, 30, 20});
    auto run = [&](int flags) {
        return Series{dftu_series_rank(s.handle(), DFTU_RANK_AVERAGE, flags)};
    };
    Series desc = run(DFTU_RANK_FLAG_DESCENDING);
    CHECK(desc.data<double>()[0] == 3.0);
    CHECK(desc.data<double>()[1] == 1.0);
    Series both = run(DFTU_RANK_FLAG_DESCENDING | DFTU_RANK_FLAG_PCT);
    CHECK(both.data<double>()[0] == doctest::Approx(1.0));
    CHECK(both.data<double>()[1] == doctest::Approx(1.0 / 3.0));
    Series pct = run(DFTU_RANK_FLAG_PCT);
    CHECK(pct.data<double>()[2] == doctest::Approx(2.0 / 3.0));
}
