#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/comparator/comparison_result.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace dftracer::utils::utilities::composites::dft::comparator;
using namespace dftracer::utils::utilities::composites::dft::aggregators;

static MetricStats make_stats(double mean, double m2, uint64_t total,
                              uint64_t min_val, uint64_t max_val) {
    MetricStats s;
    s.mean = mean;
    s.m2 = m2;
    s.total = total;
    s.min = min_val;
    s.max = max_val;
    return s;
}

static AggregationKey make_key(const std::string& cat, const std::string& name,
                               uint64_t pid, uint64_t time_bucket) {
    AggregationKey k;
    k.cat = cat;
    k.name = name;
    k.pid = pid;
    k.tid = 0;
    k.time_bucket = time_bucket;
    return k;
}

static AggregationMetrics make_metrics(uint64_t count, uint64_t dur_total,
                                       uint64_t size_total) {
    AggregationMetrics m;
    for (uint64_t i = 0; i < count; ++i) {
        m.update_duration(dur_total / count);
        if (size_total > 0) {
            m.update_size(size_total / count);
        }
    }
    return m;
}

static CollapsedMetrics make_collapsed(double count_mean, double dur_mean,
                                       double size_mean, double xfer_mean,
                                       double bw_mean, size_t num_windows) {
    CollapsedMetrics cm;
    cm.count_mean = count_mean;
    cm.dur_mean_of_means = dur_mean;
    cm.size_mean_of_means = size_mean;
    cm.xfer_mean = xfer_mean;
    cm.bw_mean = bw_mean;
    cm.num_windows = num_windows;
    for (size_t i = 0; i < static_cast<size_t>(count_mean); ++i) {
        cm.merged.update_duration(static_cast<uint64_t>(dur_mean));
    }
    return cm;
}

TEST_SUITE("ComputeCohensd") {
    TEST_CASE("compute_cohens_d - identical distributions") {
        MetricStats base = make_stats(50.0, 100.0, 500, 10, 100);
        MetricStats var = make_stats(50.0, 100.0, 500, 10, 100);
        double d = compute_cohens_d(base, 10, var, 10);
        CHECK(d == doctest::Approx(0.0).epsilon(1e-10));
    }

    TEST_CASE("compute_cohens_d - large difference") {
        // baseline: mean=10, m2=10 (small variance)
        // variant:  mean=100, m2=10
        MetricStats base = make_stats(10.0, 10.0, 100, 5, 15);
        MetricStats var = make_stats(100.0, 10.0, 1000, 90, 110);
        double d = compute_cohens_d(base, 10, var, 10);
        CHECK(std::abs(d) > 0.8);
    }

    TEST_CASE("compute_cohens_d - insufficient samples") {
        MetricStats base = make_stats(10.0, 50.0, 10, 5, 15);
        MetricStats var = make_stats(20.0, 50.0, 20, 10, 30);

        // n_base = 1 -> returns 0
        CHECK(compute_cohens_d(base, 1, var, 10) ==
              doctest::Approx(0.0).epsilon(1e-10));
        // n_var = 1 -> returns 0
        CHECK(compute_cohens_d(base, 10, var, 1) ==
              doctest::Approx(0.0).epsilon(1e-10));
    }

    TEST_CASE("compute_cohens_d - zero variance") {
        MetricStats base = make_stats(42.0, 0.0, 420, 42, 42);
        MetricStats var = make_stats(42.0, 0.0, 420, 42, 42);
        double d = compute_cohens_d(base, 10, var, 10);
        CHECK(d == doctest::Approx(0.0).epsilon(1e-10));
    }
}

TEST_SUITE("ClassifySignificance") {
    TEST_CASE("classify_significance - negligible") {
        CHECK(classify_significance(0.1) == Significance::NEGLIGIBLE);
    }

    TEST_CASE("classify_significance - small") {
        CHECK(classify_significance(0.3) == Significance::SMALL);
    }

    TEST_CASE("classify_significance - medium") {
        CHECK(classify_significance(0.6) == Significance::MEDIUM);
    }

    TEST_CASE("classify_significance - large") {
        CHECK(classify_significance(1.0) == Significance::LARGE);
    }

    TEST_CASE("classify_significance - negative d") {
        // abs(-0.9) = 0.9 > 0.8 -> LARGE
        CHECK(classify_significance(-0.9) == Significance::LARGE);
    }
}

TEST_SUITE("CollapseByGroup") {
    TEST_CASE("collapse_by_group - single entry") {
        std::unordered_map<AggregationKey, AggregationMetrics,
                           AggregationKeyHash>
            agg;
        agg[make_key("POSIX", "lseek64", 1, 0)] = make_metrics(5, 500, 0);

        auto result = collapse_by_group(agg);
        CHECK(result.size() == 1);
        auto it = result.begin();
        CHECK(it->second.num_windows == 1);
        CHECK(it->second.count_mean == doctest::Approx(5.0));
    }

    TEST_CASE("collapse_by_group - merges time windows") {
        std::unordered_map<AggregationKey, AggregationMetrics,
                           AggregationKeyHash>
            agg;
        // Same (cat, name, pid=1) across 3 time windows
        agg[make_key("POSIX", "open", 1, 0)] = make_metrics(10, 1000, 0);
        agg[make_key("POSIX", "open", 1, 1)] = make_metrics(20, 2000, 0);
        agg[make_key("POSIX", "open", 1, 2)] = make_metrics(30, 3000, 0);

        auto result = collapse_by_group(agg);
        REQUIRE(result.size() == 1);
        const auto& cm = result.begin()->second;
        CHECK(cm.num_windows == 3);
        // mean of per-window max counts: (10+20+30)/3 = 20
        CHECK(cm.count_mean == doctest::Approx(20.0).epsilon(0.01));
    }

    TEST_CASE("collapse_by_group - max across pids") {
        std::unordered_map<AggregationKey, AggregationMetrics,
                           AggregationKeyHash>
            agg;
        // Same (cat, name, time_bucket=0) with different pids
        agg[make_key("POSIX", "close", 1, 0)] = make_metrics(5, 500, 0);
        agg[make_key("POSIX", "close", 2, 0)] = make_metrics(15, 1500, 0);
        agg[make_key("POSIX", "close", 3, 0)] = make_metrics(10, 1000, 0);

        auto result = collapse_by_group(agg);
        REQUIRE(result.size() == 1);
        const auto& cm = result.begin()->second;
        // One window (time_bucket=0), max count across pids = 15
        CHECK(cm.num_windows == 1);
        CHECK(cm.count_mean == doctest::Approx(15.0).epsilon(0.01));
    }

    TEST_CASE("collapse_by_group - separate cat/name") {
        std::unordered_map<AggregationKey, AggregationMetrics,
                           AggregationKeyHash>
            agg;
        agg[make_key("POSIX", "read", 1, 0)] = make_metrics(10, 1000, 4096);
        agg[make_key("POSIX", "write", 1, 0)] = make_metrics(5, 500, 2048);
        agg[make_key("STDIO", "fread", 1, 0)] = make_metrics(3, 300, 1024);

        auto result = collapse_by_group(agg);
        CHECK(result.size() == 3);
    }

    TEST_CASE("collapse_by_group - bandwidth only for I/O ops") {
        std::unordered_map<AggregationKey, AggregationMetrics,
                           AggregationKeyHash>
            agg;
        // read is a data transfer op -> bw_mean > 0
        agg[make_key("POSIX", "read", 1, 0)] = make_metrics(10, 1000, 40960);
        // lseek64 is not -> bw_mean = 0
        agg[make_key("POSIX", "lseek64", 1, 0)] = make_metrics(10, 500, 0);

        auto result = collapse_by_group(agg);
        REQUIRE(result.size() == 2);

        for (const auto& [k, cm] : result) {
            if (k.name == "read") {
                CHECK(cm.bw_mean > 0.0);
            } else {
                CHECK(cm.bw_mean == doctest::Approx(0.0).epsilon(1e-10));
            }
        }
    }
}

TEST_SUITE("CompareMetrics") {
    TEST_CASE("compare_metrics - count metric") {
        auto base = make_collapsed(100.0, 500.0, 0.0, 0.0, 0.0, 1);
        auto var = make_collapsed(150.0, 500.0, 0.0, 0.0, 0.0, 1);

        auto results = compare_metrics(base, var, {"count"}, {});
        REQUIRE(results.size() == 1);
        const auto& cmp = results[0];
        CHECK(cmp.metric_name == "count");
        CHECK(cmp.delta == doctest::Approx(50.0).epsilon(0.01));
        CHECK(cmp.pct_change == doctest::Approx(50.0).epsilon(0.01));
    }

    TEST_CASE("compare_metrics - duration metric") {
        auto base = make_collapsed(10.0, 200.0, 0.0, 0.0, 0.0, 1);
        auto var = make_collapsed(10.0, 300.0, 0.0, 0.0, 0.0, 1);

        auto results = compare_metrics(base, var, {"duration"}, {});
        // At least the mean entry; no percentile entries since percentiles={}
        REQUIRE_FALSE(results.empty());
        auto it = std::find_if(results.begin(), results.end(),
                               [](const MetricComparison& c) {
                                   return c.metric_name == "dur_mean";
                               });
        REQUIRE(it != results.end());
        CHECK(it->baseline_value == doctest::Approx(200.0).epsilon(0.01));
        CHECK(it->variant_value == doctest::Approx(300.0).epsilon(0.01));
    }

    TEST_CASE("compare_metrics - size metric") {
        auto base = make_collapsed(10.0, 100.0, 512.0, 0.0, 0.0, 1);
        auto var = make_collapsed(10.0, 100.0, 1024.0, 0.0, 0.0, 1);

        auto results = compare_metrics(base, var, {"size"}, {});
        REQUIRE_FALSE(results.empty());
        auto it = std::find_if(results.begin(), results.end(),
                               [](const MetricComparison& c) {
                                   return c.metric_name == "size_mean";
                               });
        REQUIRE(it != results.end());
        CHECK(it->baseline_value == doctest::Approx(512.0).epsilon(0.01));
        CHECK(it->variant_value == doctest::Approx(1024.0).epsilon(0.01));
    }

    TEST_CASE("compare_metrics - transfer_size skipped when zero") {
        auto base = make_collapsed(10.0, 100.0, 0.0, 0.0, 0.0, 1);
        auto var = make_collapsed(10.0, 100.0, 0.0, 0.0, 0.0, 1);

        auto results = compare_metrics(base, var, {"transfer_size"}, {});
        auto it = std::find_if(results.begin(), results.end(),
                               [](const MetricComparison& c) {
                                   return c.metric_name == "transfer_size";
                               });
        CHECK(it == results.end());
    }

    TEST_CASE("compare_metrics - bandwidth regression is decrease") {
        // Higher baseline bw -> variant is slower -> regression
        auto base = make_collapsed(10.0, 100.0, 0.0, 0.0, 1000.0, 1);
        auto var = make_collapsed(10.0, 100.0, 0.0, 0.0, 500.0, 1);

        auto results = compare_metrics(base, var, {"bandwidth"}, {});
        REQUIRE(results.size() == 1);
        CHECK(results[0].metric_name == "bandwidth");
        CHECK(results[0].is_regression == true);
    }

    TEST_CASE("compare_metrics - zero baseline pct_change") {
        // baseline count = 0 (merged has no updates), variant count = 100
        CollapsedMetrics base;
        base.count_mean = 0.0;
        base.num_windows = 1;

        CollapsedMetrics var;
        var.count_mean = 100.0;
        var.num_windows = 1;
        for (int i = 0; i < 100; ++i) {
            var.merged.update_duration(10);
        }

        auto results = compare_metrics(base, var, {"count"}, {});
        REQUIRE(results.size() == 1);
        CHECK(results[0].pct_change == doctest::Approx(100.0).epsilon(0.01));
    }
}
