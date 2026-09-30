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

const double NAN_ = std::nan("");

Series floats(const std::vector<double>& v,
              const std::vector<std::uint8_t>& valid = {}) {
    return Series::flat_f64(v.data(), static_cast<std::int64_t>(v.size()),
                            valid.empty() ? nullptr : valid.data());
}

// Reference: drop the missing rows, rank the rest, scatter back.
std::vector<double> reference(const std::vector<double>& x, RankMethod method,
                              bool descending, bool pct) {
    std::vector<double> present;
    std::vector<std::size_t> at;
    for (std::size_t i = 0; i < x.size(); ++i)
        if (!std::isnan(x[i])) {
            present.push_back(x[i]);
            at.push_back(i);
        }
    std::vector<double> out(x.size(), NAN_);
    if (present.empty()) return out;
    Series r = floats(present).rank(method, descending, pct);
    for (std::size_t t = 0; t < at.size(); ++t)
        out[at[t]] = r.data<double>()[t];
    return out;
}

}  // namespace

TEST_CASE("NaN and null are unranked") {
    // 1.0, NaN, 3.0, null, 2.0
    Series r = floats({1.0, NAN_, 3.0, 0.0, 2.0}, {0x15})
                   .rank(RankMethod::Average, false, false);
    REQUIRE(r.valid());
    CHECK(r.data<double>()[0] == 1.0);
    CHECK(r.is_null(1));
    CHECK(r.data<double>()[2] == 3.0);
    CHECK(r.is_null(3));
    CHECK(r.data<double>()[4] == 2.0);
}

TEST_CASE("percentile rank skips NaN") {
    Series r = floats({1.0, NAN_, 3.0, 0.0, 2.0}, {0x15})
                   .rank(RankMethod::Average, false, true);
    REQUIRE(r.valid());
    CHECK(r.data<double>()[0] == doctest::Approx(1.0 / 3.0));
    CHECK(r.is_null(1));
    CHECK(r.data<double>()[2] == doctest::Approx(1.0));
    CHECK(r.is_null(3));
    CHECK(r.data<double>()[4] == doctest::Approx(2.0 / 3.0));
}

TEST_CASE("dense rank skips NaN") {
    Series r = floats({1.0, NAN_, 3.0, 0.0, 2.0}, {0x15})
                   .rank(RankMethod::Dense, false, false);
    REQUIRE(r.valid());
    CHECK(r.data<double>()[0] == 1.0);
    CHECK(r.is_null(1));
    CHECK(r.data<double>()[2] == 3.0);
    CHECK(r.data<double>()[4] == 2.0);
}

TEST_CASE("a column of only NaN has only null ranks") {
    Series r = floats({NAN_, NAN_}).rank(RankMethod::Average, false, true);
    REQUIRE(r.valid());
    CHECK(r.is_null(0));
    CHECK(r.is_null(1));
}

TEST_CASE("a zero over zero slope is unranked") {
    volatile double zero = 0.0;
    Series r =
        floats({0.5, zero / zero, 1.5}).rank(RankMethod::Average, false, true);
    REQUIRE(r.valid());
    CHECK(r.data<double>()[0] == doctest::Approx(0.5));
    CHECK(r.is_null(1));
    CHECK(r.data<double>()[2] == doctest::Approx(1.0));
}

TEST_CASE("every method and direction equals ranking the present rows") {
    std::uint64_t s = 88172645463325252ULL;
    auto next = [&] {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    };
    for (int round = 0; round < 40; ++round) {
        std::vector<double> x(60);
        for (auto& v : x)
            v = (next() % 5 == 0) ? NAN_ : static_cast<double>(next() % 9);
        for (RankMethod m :
             {RankMethod::Average, RankMethod::Min, RankMethod::Max,
              RankMethod::Dense, RankMethod::Ordinal})
            for (bool desc : {false, true})
                for (bool pct : {false, true}) {
                    const auto want = reference(x, m, desc, pct);
                    Series r = floats(x).rank(m, desc, pct);
                    REQUIRE(r.valid());
                    for (std::size_t i = 0; i < x.size(); ++i) {
                        if (std::isnan(want[i]))
                            CHECK(r.is_null(static_cast<std::int64_t>(i)));
                        else
                            CHECK(r.data<double>()[i] ==
                                  doctest::Approx(want[i]));
                    }
                }
    }
}
