#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_query_utility.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <string>

using namespace dftracer::utils::utilities::composites::dft::statistics;
using namespace dftracer::utils::utilities::composites::dft::indexing;

static TraceStatistics make_test_stats() {
    TraceStatistics ts;
    ts.success = true;
    ts.num_chunks = 2;
    ts.file_path = "/test/file.pfw.gz";
    ts.index_path = "/test/file.pfw.gz.idx";

    // Simulate a variety of events
    ts.merged.update_from_event("read", "POSIX", 1, 1, 1000, 100);
    ts.merged.update_from_event("read", "POSIX", 1, 1, 2000, 200);
    ts.merged.update_from_event("write", "POSIX", 1, 2, 3000, 300);
    ts.merged.update_from_event("open", "storage", 2, 1, 4000, 50);
    ts.merged.update_from_event("close", "storage", 2, 1, 5000, 10);

    return ts;
}

TEST_SUITE("StatisticsQueryUtility") {
    TEST_CASE("Query - SUMMARY populates all fields") {
        auto stats = make_test_stats();

        StatisticsQueryInput input;
        input.stats = stats;
        input.query_type = StatisticsQueryType::SUMMARY;

        StatisticsQueryUtility query;
        auto output = query.process(input).get();

        CHECK(output.total_events == 5);
        CHECK(output.query_type_name == "summary");
        CHECK(!output.results.empty());
        CHECK(output.duration_count == 5);
        CHECK(output.duration_mean_us == doctest::Approx(132.0));
        CHECK(output.time_span_seconds > 0.0);
    }

    TEST_CASE("Query - CATEGORIES returns sorted desc") {
        auto stats = make_test_stats();

        StatisticsQueryInput input;
        input.stats = stats;
        input.query_type = StatisticsQueryType::CATEGORIES;

        StatisticsQueryUtility query;
        auto output = query.process(input).get();

        CHECK(output.results.size() == 2);
        // POSIX has 3 events, storage has 2
        CHECK(output.results[0].first == "POSIX");
        CHECK(output.results[0].second == 3);
        CHECK(output.results[1].first == "storage");
        CHECK(output.results[1].second == 2);
    }

    TEST_CASE("Query - NAMES returns sorted desc") {
        auto stats = make_test_stats();

        StatisticsQueryInput input;
        input.stats = stats;
        input.query_type = StatisticsQueryType::NAMES;

        StatisticsQueryUtility query;
        auto output = query.process(input).get();

        CHECK(output.results.size() == 4);
        // read=2, write=1, open=1, close=1
        CHECK(output.results[0].first == "read");
        CHECK(output.results[0].second == 2);
    }

    TEST_CASE("Query - PID_TIDS") {
        auto stats = make_test_stats();

        StatisticsQueryInput input;
        input.stats = stats;
        input.query_type = StatisticsQueryType::PID_TIDS;

        StatisticsQueryUtility query;
        auto output = query.process(input).get();

        CHECK(!output.results.empty());
        // 1:1=2, 1:2=1, 2:1=2
        bool found_1_1 = false;
        for (const auto& [name, count] : output.results) {
            if (name == "1:1") {
                CHECK(count == 2);
                found_1_1 = true;
            }
        }
        CHECK(found_1_1);
    }

    TEST_CASE("Query - TIME_RANGE") {
        auto stats = make_test_stats();

        StatisticsQueryInput input;
        input.stats = stats;
        input.query_type = StatisticsQueryType::TIME_RANGE;

        StatisticsQueryUtility query;
        auto output = query.process(input).get();

        CHECK(output.min_timestamp_us == 1000);
        CHECK(output.max_timestamp_us == 5010);  // 5000 + 10
        CHECK(output.time_span_seconds > 0.0);
    }

    TEST_CASE("Query - DURATION_STATS") {
        auto stats = make_test_stats();

        StatisticsQueryInput input;
        input.stats = stats;
        input.query_type = StatisticsQueryType::DURATION_STATS;

        StatisticsQueryUtility query;
        auto output = query.process(input).get();

        CHECK(output.duration_count == 5);
        CHECK(output.duration_mean_us == doctest::Approx(132.0));
        CHECK(output.duration_stddev_us > 0.0);
        CHECK(output.duration_min_us == 10);
        CHECK(output.duration_max_us == 300);
    }

    TEST_CASE("Query - TOP_N_NAMES") {
        auto stats = make_test_stats();

        StatisticsQueryInput input;
        input.stats = stats;
        input.query_type = StatisticsQueryType::TOP_N_NAMES;
        input.top_n = 2;

        StatisticsQueryUtility query;
        auto output = query.process(input).get();

        CHECK(output.results.size() == 2);
        // Top 2: read=2, then one of write/open/close=1
        CHECK(output.results[0].first == "read");
        CHECK(output.results[0].second == 2);
        CHECK(output.results[1].second == 1);
    }

    TEST_CASE("Query - TOP_N_CATEGORIES") {
        auto stats = make_test_stats();

        StatisticsQueryInput input;
        input.stats = stats;
        input.query_type = StatisticsQueryType::TOP_N_CATEGORIES;
        input.top_n = 1;

        StatisticsQueryUtility query;
        auto output = query.process(input).get();

        CHECK(output.results.size() == 1);
        CHECK(output.results[0].first == "POSIX");
        CHECK(output.results[0].second == 3);
    }

    TEST_CASE("Query - to_json produces valid JSON for each query type") {
        auto stats = make_test_stats();

        StatisticsQueryType types[] = {
            StatisticsQueryType::SUMMARY,
            StatisticsQueryType::CATEGORIES,
            StatisticsQueryType::NAMES,
            StatisticsQueryType::TIME_RANGE,
            StatisticsQueryType::DURATION_STATS,
            StatisticsQueryType::TOP_N_NAMES,
        };

        StatisticsQueryUtility query;

        for (auto qt : types) {
            StatisticsQueryInput input;
            input.stats = stats;
            input.query_type = qt;
            input.top_n = 5;

            auto output = query.process(input).get();
            std::string json = output.to_json();

            simdjson::dom::parser parser;
            auto result = parser.parse(json);
            REQUIRE(!result.error());

            auto root = result.value_unsafe();
            REQUIRE(root.is_object());

            // query_type field should always be present
            CHECK(!root["query_type"].error());
            CHECK(!root["total_events"].error());
        }
    }
}
