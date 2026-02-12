#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>

using namespace dftracer::utils::utilities::composites::dft::aggregators;

TEST_SUITE("AggregationConfig") {
    TEST_CASE("AggregationConfig - No filters passes everything") {
        AggregationConfig config;
        CHECK(config.passes_filters("any_cat", "any_name"));
        CHECK(config.passes_filters("", ""));
        CHECK(config.passes_filters("io", "read"));
    }

    TEST_CASE("AggregationConfig - Include categories only") {
        AggregationConfig config;
        config.include_categories = {"io", "compute"};

        SUBCASE("Matching category passes") {
            CHECK(config.passes_filters("io", "read"));
            CHECK(config.passes_filters("compute", "matmul"));
        }

        SUBCASE("Non-matching category fails") {
            CHECK_FALSE(config.passes_filters("network", "send"));
            CHECK_FALSE(config.passes_filters("memory", "alloc"));
        }
    }

    TEST_CASE("AggregationConfig - Exclude categories only") {
        AggregationConfig config;
        config.exclude_categories = {"debug", "trace"};

        SUBCASE("Matching category fails") {
            CHECK_FALSE(config.passes_filters("debug", "log"));
            CHECK_FALSE(config.passes_filters("trace", "span"));
        }

        SUBCASE("Non-matching category passes") {
            CHECK(config.passes_filters("io", "read"));
            CHECK(config.passes_filters("compute", "matmul"));
        }
    }

    TEST_CASE("AggregationConfig - Include + exclude categories") {
        AggregationConfig config;
        config.include_categories = {"io", "debug"};
        config.exclude_categories = {"debug"};

        // "io" is included and not excluded -> passes
        CHECK(config.passes_filters("io", "read"));
        // "debug" is included but also excluded -> exclude takes precedence
        CHECK_FALSE(config.passes_filters("debug", "log"));
        // "compute" is not included -> fails
        CHECK_FALSE(config.passes_filters("compute", "matmul"));
    }

    TEST_CASE("AggregationConfig - Include names only") {
        AggregationConfig config;
        config.include_names = {"read", "write"};

        SUBCASE("Matching name passes") {
            CHECK(config.passes_filters("io", "read"));
            CHECK(config.passes_filters("io", "write"));
        }

        SUBCASE("Non-matching name fails") {
            CHECK_FALSE(config.passes_filters("io", "close"));
            CHECK_FALSE(config.passes_filters("io", "open"));
        }
    }

    TEST_CASE("AggregationConfig - Exclude names only") {
        AggregationConfig config;
        config.exclude_names = {"debug_log", "trace_span"};

        SUBCASE("Matching name fails") {
            CHECK_FALSE(config.passes_filters("any", "debug_log"));
            CHECK_FALSE(config.passes_filters("any", "trace_span"));
        }

        SUBCASE("Non-matching name passes") {
            CHECK(config.passes_filters("any", "read"));
            CHECK(config.passes_filters("any", "write"));
        }
    }

    TEST_CASE("AggregationConfig - Combined category + name filters") {
        AggregationConfig config;
        config.include_categories = {"io"};
        config.include_names = {"read"};

        // Both category and name must pass
        CHECK(config.passes_filters("io", "read"));
        CHECK_FALSE(config.passes_filters("io", "write"));     // name fails
        CHECK_FALSE(config.passes_filters("net", "read"));     // cat fails
        CHECK_FALSE(config.passes_filters("net", "write"));    // both fail
    }

    TEST_CASE("AggregationConfig - Empty include list acts as include all") {
        AggregationConfig config;
        // include_categories is empty (default) -> all categories pass
        // include_names is empty (default) -> all names pass
        config.exclude_categories = {"debug"};

        CHECK(config.passes_filters("io", "read"));
        CHECK(config.passes_filters("compute", "matmul"));
        CHECK_FALSE(config.passes_filters("debug", "log"));
    }
}
