// The aggregation tier stores central moments, so a standard deviation read
// back from an index stays accurate when the mean is large next to the spread.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/schemas/dft/agg/aggregation_metrics.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_serialization.h>
#include <dftracer/utils/index/store/layout.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using namespace dftracer::utils::index::schemas::dft::agg;

namespace {

// Durations of mean 1e9 us (about 17 minutes) and spread 1000 us.
std::vector<std::uint64_t> durations(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> z(1e9, 1000.0);
    std::vector<std::uint64_t> v(n);
    for (auto& x : v) x = static_cast<std::uint64_t>(std::llround(z(rng)));
    return v;
}

double two_pass_std(const std::vector<std::uint64_t>& v) {
    long double s = 0;
    for (auto x : v) s += static_cast<long double>(x);
    const long double mu = s / static_cast<long double>(v.size());
    long double m2 = 0;
    for (auto x : v) {
        const long double d = static_cast<long double>(x) - mu;
        m2 += d * d;
    }
    return static_cast<double>(
        std::sqrt(m2 / static_cast<long double>(v.size() - 1)));
}

double rel(double got, double want) {
    return std::fabs(got - want) / std::fabs(want);
}

}  // namespace

TEST_SUITE("VarianceTier") {
    TEST_CASE("the stored standard deviation survives a large mean") {
        const auto v = durations(200000, 1);
        AggregationMetrics m;
        for (auto x : v) m.update_duration(x, false);
        CHECK(rel(m.duration.get_stddev(), two_pass_std(v)) < 1e-9);

        const auto data = serialize_agg_value(m);
        auto back = deserialize_agg_value(data);
        CHECK(back.duration.count() == v.size());
        CHECK(rel(back.duration.get_stddev(), two_pass_std(v)) < 1e-9);

        AggMetricsFullView fv;
        REQUIRE(parse_agg_value_full_view(data, fv));
        const double sd =
            std::sqrt(fv.dur_m2 / static_cast<double>(fv.count - 1));
        CHECK(rel(sd, two_pass_std(v)) < 1e-9);
        CHECK(fv.dur_mean == doctest::Approx(1e9).epsilon(1e-6));
    }

    TEST_CASE("merged tier rows keep the standard deviation") {
        const auto v = durations(200000, 2);
        const std::size_t cut = 70001;
        AggregationMetrics a, b;
        for (std::size_t i = 0; i < v.size(); ++i)
            (i < cut ? a : b).update_duration(v[i], false);
        // Each half is stored and read back, as two rows of an index are.
        auto ra = deserialize_agg_value(serialize_agg_value(a));
        const auto rb = deserialize_agg_value(serialize_agg_value(b));
        ra.merge_from(rb);
        CHECK(ra.duration.count() == v.size());
        CHECK(rel(ra.duration.get_stddev(), two_pass_std(v)) < 1e-9);
    }

    TEST_CASE("a single event still round trips through the compact form") {
        AggregationMetrics m;
        m.update_duration(1'000'000'123ULL, false);
        const auto data = serialize_agg_value(m);
        auto back = deserialize_agg_value(data);
        CHECK(back.duration.count() == 1);
        CHECK(back.duration.total() == 1'000'000'123.0);
        CHECK(back.duration.get_stddev() == 0.0);
        AggMetricsFullView fv;
        REQUIRE(parse_agg_value_full_view(data, fv));
        CHECK(fv.dur_mean == 1'000'000'123.0);
        CHECK(fv.dur_mean_lo == 0.0);
        CHECK(fv.dur_m2 == 0.0);
    }
}
