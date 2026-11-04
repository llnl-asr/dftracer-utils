// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/schemas/dft/agg/system_metrics.h>
#include <doctest/doctest.h>

#include <cmath>
#include <limits>

using namespace dftracer::utils::index::schemas::dft::agg;

TEST_SUITE("MetricStats double path") {
    TEST_CASE("default construction") {
        MetricStats stats;
        CHECK(stats.count() == 0);
        CHECK(stats.total() == 0.0);
        CHECK(stats.mean() == 0.0);
        CHECK(stats.m2() == 0.0);
        CHECK(stats.sketch == nullptr);
    }

    TEST_CASE("single value update") {
        MetricStats stats;
        stats.update(42.5);

        CHECK(stats.count() == 1);
        CHECK(stats.total() == doctest::Approx(42.5));
        CHECK(stats.min() == doctest::Approx(42.5));
        CHECK(stats.max() == doctest::Approx(42.5));
        CHECK(stats.mean() == doctest::Approx(42.5));
        CHECK(stats.stddev() == doctest::Approx(0.0));
    }

    TEST_CASE("multiple values update") {
        MetricStats stats;
        stats.update(10.0);
        stats.update(20.0);
        stats.update(30.0);

        CHECK(stats.count() == 3);
        CHECK(stats.total() == doctest::Approx(60.0));
        CHECK(stats.min() == doctest::Approx(10.0));
        CHECK(stats.max() == doctest::Approx(30.0));
        CHECK(stats.mean() == doctest::Approx(20.0));
        CHECK(stats.stddev() == doctest::Approx(10.0));
    }

    TEST_CASE("update with percentiles") {
        MetricStats stats;
        stats.update(10.0, true);
        stats.update(20.0, true);
        stats.update(30.0, true);

        CHECK(stats.sketch != nullptr);
        CHECK(stats.sketch->quantile(0.5) ==
              doctest::Approx(20.0).epsilon(0.1));
    }

    TEST_CASE("merge_from empty into empty") {
        MetricStats a, b;
        a.merge_from(b);

        CHECK(a.count() == 0);
        CHECK(a.total() == 0.0);
    }

    TEST_CASE("merge_from populated into empty") {
        MetricStats a, b;
        b.update(10.0);
        b.update(20.0);

        a.merge_from(b);

        CHECK(a.count() == 2);
        CHECK(a.total() == doctest::Approx(30.0));
        CHECK(a.min() == doctest::Approx(10.0));
        CHECK(a.max() == doctest::Approx(20.0));
        CHECK(a.mean() == doctest::Approx(15.0));
    }

    TEST_CASE("merge_from two populated stats") {
        MetricStats a, b;
        a.update(10.0);
        a.update(20.0);
        b.update(30.0);
        b.update(40.0);

        a.merge_from(b);

        CHECK(a.count() == 4);
        CHECK(a.total() == doctest::Approx(100.0));
        CHECK(a.min() == doctest::Approx(10.0));
        CHECK(a.max() == doctest::Approx(40.0));
        CHECK(a.mean() == doctest::Approx(25.0));
    }

    TEST_CASE("merge_from with sketches") {
        MetricStats a, b;
        a.update(10.0, true);
        a.update(20.0, true);
        b.update(30.0, true);
        b.update(40.0, true);

        a.merge_from(b);

        CHECK(a.sketch != nullptr);
        CHECK(a.count() == 4);
    }

    TEST_CASE("copy construction") {
        MetricStats original;
        original.update(10.0, true);
        original.update(20.0, true);

        MetricStats copy(original);

        CHECK(copy.count() == original.count());
        CHECK(copy.total() == original.total());
        CHECK(copy.min() == original.min());
        CHECK(copy.max() == original.max());
        CHECK(copy.mean() == original.mean());
        CHECK(copy.sketch != nullptr);
        CHECK(copy.sketch != original.sketch);
    }
}

TEST_SUITE("SystemAggregationMetrics") {
    TEST_CASE("default construction") {
        SystemAggregationMetrics metrics;
        CHECK(metrics.count == 0);
        CHECK(metrics.ts == std::numeric_limits<std::uint64_t>::max());
        CHECK(metrics.te == 0);
        CHECK(metrics.metrics == nullptr);
    }

    TEST_CASE("update_metric creates metrics map") {
        SystemAggregationMetrics metrics;
        metrics.update_metric("cpu_usage", 50.0);

        CHECK(metrics.metrics != nullptr);
        CHECK(metrics.metrics->size() == 1);
        CHECK(metrics.metrics->at("cpu_usage").count() == 1);
        CHECK(metrics.metrics->at("cpu_usage").mean() == doctest::Approx(50.0));
    }

    TEST_CASE("update_metric multiple metrics") {
        SystemAggregationMetrics metrics;
        metrics.update_metric("cpu_usage", 50.0);
        metrics.update_metric("memory_usage", 70.0);
        metrics.update_metric("cpu_usage", 60.0);

        CHECK(metrics.metrics->size() == 2);
        CHECK(metrics.metrics->at("cpu_usage").count() == 2);
        CHECK(metrics.metrics->at("cpu_usage").mean() == doctest::Approx(55.0));
        CHECK(metrics.metrics->at("memory_usage").count() == 1);
    }

    TEST_CASE("update_timestamp") {
        SystemAggregationMetrics metrics;
        metrics.update_timestamp(1000);
        metrics.update_timestamp(500);
        metrics.update_timestamp(1500);

        CHECK(metrics.ts == 500);
        CHECK(metrics.te == 1500);
    }

    TEST_CASE("merge_from empty into empty") {
        SystemAggregationMetrics a, b;
        a.merge_from(b);

        CHECK(a.count == 0);
        CHECK(a.metrics == nullptr);
    }

    TEST_CASE("merge_from populated into empty") {
        SystemAggregationMetrics a, b;
        b.count = 2;
        b.ts = 100;
        b.te = 200;
        b.update_metric("cpu", 50.0);

        a.merge_from(b);

        CHECK(a.count == 2);
        CHECK(a.ts == 100);
        CHECK(a.te == 200);
        CHECK(a.metrics != nullptr);
        CHECK(a.metrics->at("cpu").count() == 1);
    }

    TEST_CASE("merge_from two populated metrics") {
        SystemAggregationMetrics a, b;
        a.count = 2;
        a.ts = 100;
        a.te = 200;
        a.update_metric("cpu", 40.0);
        a.update_metric("cpu", 60.0);

        b.count = 2;
        b.ts = 50;
        b.te = 250;
        b.update_metric("cpu", 50.0);
        b.update_metric("memory", 80.0);

        a.merge_from(b);

        CHECK(a.count == 4);
        CHECK(a.ts == 50);
        CHECK(a.te == 250);
        CHECK(a.metrics->size() == 2);
        CHECK(a.metrics->at("cpu").count() == 3);
        CHECK(a.metrics->at("memory").count() == 1);
    }
}
