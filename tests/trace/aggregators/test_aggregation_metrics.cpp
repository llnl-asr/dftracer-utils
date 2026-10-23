#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/schemas/dft/agg/aggregation_metrics.h>
#include <doctest/doctest.h>

#include <cmath>

using namespace dftracer::utils::index::schemas::dft::agg;

TEST_SUITE("MetricStats") {
    TEST_CASE("MetricStats - Single value") {
        MetricStats stats;
        stats.update(42.0);

        CHECK(stats.count() == 1);
        CHECK(stats.mean() == doctest::Approx(42.0));
        CHECK(stats.get_stddev() == 0.0);
        CHECK(stats.total() == 42);
        CHECK(stats.min() == 42);
        CHECK(stats.max() == 42);
    }

    TEST_CASE("MetricStats - Two values") {
        MetricStats stats;
        stats.update(10.0);
        stats.update(20.0);

        CHECK(stats.count() == 2);
        CHECK(stats.mean() == doctest::Approx(15.0));
        CHECK(stats.total() == 30);
        CHECK(stats.min() == 10);
        CHECK(stats.max() == 20);

        // stddev = sqrt(((10-15)^2 + (20-15)^2) / 1) = sqrt(50) ~ 7.071
        double stddev = stats.get_stddev();
        CHECK(stddev == doctest::Approx(std::sqrt(50.0)).epsilon(0.001));
    }

    TEST_CASE("MetricStats - Known distribution") {
        // Values: [2, 4, 4, 4, 5, 5, 7, 9]
        // Mean = 40/8 = 5.0
        MetricStats stats;
        std::vector<std::uint64_t> values = {2, 4, 4, 4, 5, 5, 7, 9};
        for (auto v : values) {
            stats.update(v);
        }

        CHECK(stats.count() == 8);
        CHECK(stats.mean() == doctest::Approx(5.0));
        CHECK(stats.total() == 40);
        CHECK(stats.min() == 2);
        CHECK(stats.max() == 9);

        // Sample stddev = sqrt(sum((x-mean)^2) / (n-1))
        // = sqrt((9+1+1+1+0+0+4+16)/7) = sqrt(32/7) ~ 2.138
        double stddev = stats.get_stddev();
        CHECK(stddev == doctest::Approx(std::sqrt(32.0 / 7.0)).epsilon(0.01));
    }

    TEST_CASE("MetricStats - Identical values") {
        MetricStats stats;
        for (std::uint64_t i = 0; i < 10; ++i) {
            stats.update(5.0);
        }

        CHECK(stats.count() == 10);
        CHECK(stats.mean() == doctest::Approx(5.0));
        CHECK(stats.get_stddev() == doctest::Approx(0.0).epsilon(1e-10));
        CHECK(stats.get_skewness() == doctest::Approx(0.0).epsilon(1e-10));
        CHECK(stats.get_kurtosis() == doctest::Approx(0.0).epsilon(1e-10));
    }

    TEST_CASE("MetricStats - Merge equivalence") {
        // Single-pass
        MetricStats single;
        std::vector<std::uint64_t> all_values = {2, 4, 6, 8, 10, 12, 14, 16};
        for (auto v : all_values) {
            single.update(v);
        }

        // Split into two halves
        MetricStats first_half;
        for (std::uint64_t i = 0; i < 4; ++i) {
            first_half.update(all_values[i]);
        }

        MetricStats second_half;
        for (std::uint64_t i = 0; i < 4; ++i) {
            second_half.update(all_values[i + 4]);
        }

        first_half.merge_from(second_half);

        CHECK(first_half.count() == single.count());
        CHECK(first_half.mean() ==
              doctest::Approx(single.mean()).epsilon(0.001));
        CHECK(first_half.total() == single.total());
        CHECK(first_half.min() == single.min());
        CHECK(first_half.max() == single.max());
        CHECK(first_half.get_stddev() ==
              doctest::Approx(single.get_stddev()).epsilon(0.01));
    }

    TEST_CASE("MetricStats - Merge with empty") {
        MetricStats stats;
        stats.update(10.0);
        stats.update(20.0);

        MetricStats empty_stats;

        double mean_before = stats.mean();
        double total_before = stats.total();
        std::uint64_t count_before = stats.count();

        stats.merge_from(empty_stats);

        CHECK(stats.count() == count_before);
        CHECK(stats.mean() == doctest::Approx(mean_before));
        CHECK(stats.total() == total_before);
    }

    TEST_CASE("MetricStats - Percentile integration") {
        MetricStats stats;
        for (std::uint64_t i = 1; i <= 100; ++i) {
            stats.update(i, true);  // compute_percentiles = true
        }

        CHECK(stats.count() == 100);
        CHECK(stats.sketch != nullptr);
        CHECK_FALSE(stats.sketch->empty());
        REQUIRE(stats.sketch != nullptr);
        CHECK(stats.sketch->count() == 100);
        double p50 = stats.sketch->quantile(0.5);
        CHECK(p50 == doctest::Approx(50.0).epsilon(0.05));
    }
}
