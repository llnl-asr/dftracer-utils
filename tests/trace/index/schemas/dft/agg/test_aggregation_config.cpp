// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_config.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::trace::index::schemas::dft::agg;

TEST_SUITE("AggregationConfig") {
    TEST_CASE("AggregationConfig - Default values") {
        AggregationConfig config;
        CHECK(config.time_interval_us == 1000000);
        CHECK(config.use_relative_time == false);
        CHECK(config.compute_statistics == true);
        CHECK(config.compute_percentiles == false);
    }

    TEST_CASE("AggregationConfig - group_by_file changes the hash") {
        AggregationConfig with_files;
        AggregationConfig without_files;
        without_files.group_by_file = false;

        REQUIRE(with_files.group_by_file);
        CHECK(with_files.params_hash() != without_files.params_hash());
    }

    TEST_CASE("AggregationConfig - grain fields change the hash") {
        const AggregationConfig base;

        AggregationConfig coarser = base;
        coarser.time_interval_us = base.time_interval_us * 10;
        CHECK(base.params_hash() != coarser.params_hash());

        AggregationConfig extra_keys = base;
        extra_keys.extra_group_keys.push_back("epoch");
        CHECK(base.params_hash() != extra_keys.params_hash());

        AggregationConfig pct = base;
        pct.compute_percentiles = !base.compute_percentiles;
        CHECK(base.params_hash() != pct.params_hash());

        AggregationConfig normalized = base;
        normalized.normalize_time = !base.normalize_time;
        CHECK(base.params_hash() != normalized.params_hash());

        // Strings hash with their lengths, so the same text split
        // differently is a different config.
        AggregationConfig split_a = base;
        split_a.extra_group_keys = {"ab", "c"};
        AggregationConfig split_b = base;
        split_b.extra_group_keys = {"a", "bc"};
        CHECK(split_a.params_hash() != split_b.params_hash());

        AggregationConfig same = base;
        CHECK(base.params_hash() == same.params_hash());
    }

    TEST_CASE(
        "AggregationConfig - a percentiles build covers the same build "
        "without") {
        AggregationConfig want;
        AggregationConfig stored = want;
        stored.compute_percentiles = true;
        stored.sketch_accuracy = 0.05;
        stored.percentiles = {0.5, 0.99};

        CHECK(stored.covers(want));
        CHECK(stored.covers(stored));
        CHECK(want.covers(want));
        CHECK_FALSE(want.covers(stored));

        AggregationConfig other_pct = stored;
        other_pct.percentiles = {0.5};
        CHECK_FALSE(stored.covers(other_pct));

        AggregationConfig other_interval = want;
        other_interval.time_interval_us = want.time_interval_us * 10;
        CHECK_FALSE(stored.covers(other_interval));

        AggregationConfig other_keys = want;
        other_keys.extra_group_keys.push_back("epoch");
        CHECK_FALSE(stored.covers(other_keys));

        AggregationConfig no_stats = want;
        no_stats.compute_statistics = false;
        CHECK_FALSE(stored.covers(no_stats));
    }
}
