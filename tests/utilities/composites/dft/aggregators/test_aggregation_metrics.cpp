#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_metrics.h>
#include <doctest/doctest.h>

#include <cmath>

using namespace dftracer::utils::utilities::composites::dft::aggregators;

TEST_SUITE("MetricStats") {
    TEST_CASE("MetricStats - Single value") {
        MetricStats stats;
        stats.update(42);

        CHECK(stats.count == 1);
        CHECK(stats.mean == doctest::Approx(42.0));
        CHECK(stats.get_stddev() == 0.0);
        CHECK(stats.total == 42);
        CHECK(stats.min == 42);
        CHECK(stats.max == 42);
    }

    TEST_CASE("MetricStats - Two values") {
        MetricStats stats;
        stats.update(10);
        stats.update(20);

        CHECK(stats.count == 2);
        CHECK(stats.mean == doctest::Approx(15.0));
        CHECK(stats.total == 30);
        CHECK(stats.min == 10);
        CHECK(stats.max == 20);

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

        CHECK(stats.count == 8);
        CHECK(stats.mean == doctest::Approx(5.0));
        CHECK(stats.total == 40);
        CHECK(stats.min == 2);
        CHECK(stats.max == 9);

        // Sample stddev = sqrt(sum((x-mean)^2) / (n-1))
        // = sqrt((9+1+1+1+0+0+4+16)/7) = sqrt(32/7) ~ 2.138
        double stddev = stats.get_stddev();
        CHECK(stddev == doctest::Approx(std::sqrt(32.0 / 7.0)).epsilon(0.01));
    }

    TEST_CASE("MetricStats - Identical values") {
        MetricStats stats;
        for (std::uint64_t i = 0; i < 10; ++i) {
            stats.update(5);
        }

        CHECK(stats.count == 10);
        CHECK(stats.mean == doctest::Approx(5.0));
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

        CHECK(first_half.count == single.count);
        CHECK(first_half.mean == doctest::Approx(single.mean).epsilon(0.001));
        CHECK(first_half.total == single.total);
        CHECK(first_half.min == single.min);
        CHECK(first_half.max == single.max);
        CHECK(first_half.get_stddev() ==
              doctest::Approx(single.get_stddev()).epsilon(0.01));
    }

    TEST_CASE("MetricStats - Merge with empty") {
        MetricStats stats;
        stats.update(10);
        stats.update(20);

        MetricStats empty_stats;

        double mean_before = stats.mean;
        std::uint64_t total_before = stats.total;
        std::uint64_t count_before = stats.count;

        stats.merge_from(empty_stats);

        CHECK(stats.count == count_before);
        CHECK(stats.mean == doctest::Approx(mean_before));
        CHECK(stats.total == total_before);
    }

    TEST_CASE("MetricStats - Percentile integration") {
        MetricStats stats;
        for (std::uint64_t i = 1; i <= 100; ++i) {
            stats.update(i, true);  // compute_percentiles = true
        }

        CHECK(stats.count == 100);
        CHECK(stats.sketch != nullptr);
        CHECK_FALSE(stats.sketch->empty());
        REQUIRE(stats.sketch != nullptr);
        CHECK(stats.sketch->count() == 100);
        double p50 = stats.sketch->quantile(0.5);
        CHECK(p50 == doctest::Approx(50.0).epsilon(0.05));
    }
}

TEST_SUITE("AggregationMetrics") {
    TEST_CASE("AggregationMetrics - update_duration and update_size") {
        AggregationMetrics metrics;

        metrics.update_duration(100);
        CHECK(metrics.count == 1);
        CHECK(metrics.duration.count == 1);
        CHECK(metrics.duration.total == 100);
        CHECK(metrics.duration.min == 100);
        CHECK(metrics.duration.max == 100);

        metrics.update_duration(200);
        CHECK(metrics.count == 2);
        CHECK(metrics.duration.count == 2);
        CHECK(metrics.duration.total == 300);

        metrics.update_size(50);
        CHECK(metrics.size.count == 1);
        CHECK(metrics.size.total == 50);
        CHECK(metrics.size.min == 50);
        CHECK(metrics.size.max == 50);

        metrics.update_size(150);
        CHECK(metrics.size.count == 2);
        CHECK(metrics.size.total == 200);
    }

    TEST_CASE("AggregationMetrics - update_timestamp") {
        AggregationMetrics metrics;

        metrics.update_timestamp(1000, 100);
        CHECK(metrics.ts == 1000);
        CHECK(metrics.te == 1100);

        metrics.update_timestamp(500, 200);
        CHECK(metrics.ts == 500);   // min ts
        CHECK(metrics.te == 1100);  // max te stays

        metrics.update_timestamp(2000, 500);
        CHECK(metrics.ts == 500);
        CHECK(metrics.te == 2500);  // new max te
    }

    TEST_CASE("AggregationMetrics - update_timestamp_clamped") {
        AggregationMetrics metrics;

        std::uint64_t bucket_start = 1000;
        std::uint64_t bucket_size = 500;

        // Event entirely within bucket
        metrics.update_timestamp_clamped(1100, 100, bucket_start, bucket_size);
        CHECK(metrics.ts == 1100);
        CHECK(metrics.te == 1200);

        // Event starts before bucket - ts clamped to bucket_start
        metrics.update_timestamp_clamped(800, 400, bucket_start, bucket_size);
        CHECK(metrics.ts == 1000);  // clamped to bucket_start
        CHECK(metrics.te == 1200);  // 1200 from previous, 800+400=1200

        // Event ends after bucket - te clamped to bucket_end
        metrics.update_timestamp_clamped(1400, 300, bucket_start, bucket_size);
        CHECK(metrics.ts == 1000);
        CHECK(metrics.te == 1500);  // clamped to bucket_start + bucket_size
    }

    TEST_CASE("AggregationMetrics - update_custom_metric") {
        AggregationMetrics metrics;

        metrics.update_duration(100);  // increment count to 1
        metrics.update_custom_metric("bytes_read", 1024);
        REQUIRE(metrics.custom_metrics != nullptr);
        CHECK(metrics.custom_metrics->count("bytes_read") == 1);
        CHECK((*metrics.custom_metrics)["bytes_read"].count == 1);
        CHECK((*metrics.custom_metrics)["bytes_read"].total == 1024);

        metrics.update_duration(200);  // count = 2
        metrics.update_custom_metric("bytes_read", 2048);
        CHECK((*metrics.custom_metrics)["bytes_read"].count == 2);
        CHECK((*metrics.custom_metrics)["bytes_read"].total == 3072);
    }

    TEST_CASE("AggregationMetrics - sparse custom metrics have correct count") {
        AggregationMetrics metrics;

        // 3 events, but only 2 have the custom field
        metrics.update_duration(100);
        metrics.update_custom_metric("bytes_read", 1024);

        metrics.update_duration(200);
        // no bytes_read for this event

        metrics.update_duration(300);
        metrics.update_custom_metric("bytes_read", 2048);

        CHECK(metrics.count == 3);
        CHECK(metrics.duration.count == 3);
        CHECK((*metrics.custom_metrics)["bytes_read"].count == 2);
        CHECK((*metrics.custom_metrics)["bytes_read"].mean ==
              doctest::Approx(1536.0));  // (1024+2048)/2
    }

    TEST_CASE("AggregationMetrics - merge_from") {
        AggregationMetrics a, b;

        a.update_duration(100);
        a.update_duration(200);
        a.update_size(50);
        a.update_size(150);
        a.update_timestamp(1000, 100);
        a.update_custom_metric("io_ops", 10);
        a.update_custom_metric("io_ops", 20);

        b.update_duration(300);
        b.update_size(250);
        b.update_timestamp(500, 200);
        b.update_custom_metric("io_ops", 30);

        a.merge_from(b);

        CHECK(a.count == 3);
        CHECK(a.duration.count == 3);
        CHECK(a.duration.total == 600);  // 100+200+300
        CHECK(a.size.count == 3);
        CHECK(a.size.total == 450);      // 50+150+250
        CHECK(a.ts == 500);              // min of 1000, 500
        CHECK(a.te == 1100);             // max of 1100, 700
        REQUIRE(a.custom_metrics != nullptr);
        CHECK((*a.custom_metrics)["io_ops"].count == 3);
        CHECK((*a.custom_metrics)["io_ops"].total == 60);
    }

    TEST_CASE("AggregationMetrics - merge sparse custom metrics") {
        AggregationMetrics a, b;

        // a has 2 events, 1 with custom metric
        a.update_duration(100);
        a.update_custom_metric("bytes", 500);
        a.update_duration(200);

        // b has 1 event with custom metric
        b.update_duration(300);
        b.update_custom_metric("bytes", 1000);

        a.merge_from(b);

        CHECK(a.count == 3);
        auto& bytes = (*a.custom_metrics)["bytes"];
        CHECK(bytes.count == 2);  // only 2 events had bytes, not 3
        CHECK(bytes.total == 1500);
        CHECK(bytes.mean == doctest::Approx(750.0));
    }

    TEST_CASE("AggregationMetrics - get_stddev via MetricStats") {
        AggregationMetrics metrics;
        metrics.update_duration(10);
        metrics.update_duration(20);
        metrics.update_size(30);
        metrics.update_size(40);

        CHECK(metrics.duration.get_stddev() ==
              doctest::Approx(std::sqrt(50.0)).epsilon(0.01));
        CHECK(metrics.size.get_stddev() ==
              doctest::Approx(std::sqrt(50.0)).epsilon(0.01));
    }
}
