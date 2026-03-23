#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::composites::dft::aggregators;

TEST_SUITE("AggregationConfig") {
    TEST_CASE("AggregationConfig - Default values") {
        AggregationConfig config;
        CHECK(config.time_interval_us == 1000000);
        CHECK(config.use_relative_time == false);
        CHECK(config.compute_statistics == true);
        CHECK(config.compute_percentiles == false);
        CHECK(config.output_format == std::string("json"));
    }

    TEST_CASE("AggregationConfig - Valid formats") {
        CHECK(AggregationConfig::is_valid_format("json"));
        CHECK_FALSE(AggregationConfig::is_valid_format("csv"));
    }
}
