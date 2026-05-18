#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_augmentation.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::composites::dft::aggregators;

namespace {

AggregationBatch create_test_batch(std::uint64_t time_bucket,
                                   std::uint64_t count, std::uint64_t ts,
                                   std::uint64_t te) {
    AggregationBatch batch;
    AggregationKey key;
    key.cat_id = 1;
    key.name_id = 1;
    key.pid = 100;
    key.tid = 1;
    key.time_bucket = time_bucket;

    AggregationMetrics metrics;
    metrics.count = count;
    metrics.ts = ts;
    metrics.te = te;
    metrics.duration.count = count;
    metrics.duration.total = count * 1000;
    metrics.duration.mean = 1000.0;

    batch.entries.emplace_back(key, metrics);
    return batch;
}

}  // namespace

TEST_SUITE("AggregationAugmentation") {
    TEST_CASE("PassThrough - Same interval") {
        auto batch = create_test_batch(0, 100, 0, 5000);

        AugmentationConfig config{5000, 5000};
        auto result = augment_batch(batch, config);

        CHECK_FALSE(result.has_approximated_entries);
        REQUIRE(result.entries.size() == 1);
        CHECK(result.entries[0].metrics.count == 100);
        CHECK_FALSE(result.entries[0].is_approximated);
    }

    TEST_CASE("Shrink - Merge buckets") {
        AggregationBatch batch;

        for (std::uint64_t i = 0; i < 5; ++i) {
            AggregationKey key;
            key.cat_id = 1;
            key.name_id = 1;
            key.pid = 100;
            key.tid = 1;
            key.time_bucket = i;

            AggregationMetrics metrics;
            metrics.count = 20;
            metrics.ts = i * 1000;
            metrics.te = (i + 1) * 1000;
            metrics.duration.count = 20;
            metrics.duration.total = 20000;

            batch.entries.emplace_back(key, metrics);
        }

        AugmentationConfig config{1000, 5000};  // shrink 5x
        auto result = augment_batch(batch, config);

        CHECK_FALSE(result.has_approximated_entries);
        REQUIRE(result.entries.size() == 1);
        CHECK(result.entries[0].metrics.count == 100);  // 5 * 20
        CHECK(result.entries[0].key.time_bucket == 0);
    }

    TEST_CASE("Expand - Split bucket") {
        auto batch = create_test_batch(0, 100, 1000, 4000);

        AugmentationConfig config{5000, 1000};  // expand 5x
        auto result = augment_batch(batch, config);

        CHECK(result.has_approximated_entries);

        std::uint64_t total_count = 0;
        for (const auto& entry : result.entries) {
            CHECK(entry.is_approximated);
            CHECK(entry.key.time_bucket >= 1);
            CHECK(entry.key.time_bucket <= 3);
            total_count += entry.metrics.count;
            CHECK(entry.count_ci.upper > 0);
        }

        CHECK(total_count == 100);
    }

    TEST_CASE("Expand - All events at same time") {
        auto batch = create_test_batch(0, 100, 2500, 2500);

        AugmentationConfig config{5000, 1000};
        auto result = augment_batch(batch, config);

        CHECK(result.has_approximated_entries);
        REQUIRE(result.entries.size() == 1);
        CHECK(result.entries[0].key.time_bucket == 2);
        CHECK(result.entries[0].metrics.count == 100);
    }

    TEST_CASE("Poisson CI calculation") {
        SUBCASE("Count = 100") {
            auto ci = compute_poisson_ci(100.0);
            CHECK(ci.lower == doctest::Approx(80.4).epsilon(0.01));
            CHECK(ci.upper == doctest::Approx(119.6).epsilon(0.01));
        }

        SUBCASE("Count = 4") {
            auto ci = compute_poisson_ci(4.0);
            CHECK(ci.lower == doctest::Approx(0.08).epsilon(0.1));
            CHECK(ci.upper == doctest::Approx(7.92).epsilon(0.1));
        }

        SUBCASE("Count = 0") {
            auto ci = compute_poisson_ci(0.0);
            CHECK(ci.lower == 0.0);
            CHECK(ci.upper == 0.0);
        }
    }
}
