#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/statistics/trace_statistics.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <cmath>
#include <limits>
#include <string>

using namespace dftracer::utils::utilities::composites::dft::statistics;
using namespace dftracer::utils::utilities::composites::dft::indexing;

TEST_SUITE("TraceStatistics") {
    TEST_CASE("TraceStatistics - Convenience accessors") {
        TraceStatistics ts;
        ts.success = true;
        ts.num_chunks = 3;

        ts.merged.update_from_event("read", "POSIX", 1, 1, 1000000, 100);
        ts.merged.update_from_event("write", "POSIX", 1, 2, 2000000, 200);
        ts.merged.update_from_event("open", "storage", 2, 1, 3000000, 300);

        CHECK(ts.total_events() == 3);
        CHECK(ts.num_categories() == 2);
        CHECK(ts.num_unique_names() == 3);
        CHECK(ts.num_pid_tids() == 3);

        // Time span: (3000300 - 1000000) / 1e6 = 2.0003 seconds
        CHECK(ts.time_span_seconds() == doctest::Approx(2.0003));

        // Mean: (100 + 200 + 300) / 3 = 200
        CHECK(ts.duration_mean_us() == doctest::Approx(200.0));

        // Stddev: sqrt(variance), variance = sample variance of {100, 200, 300}
        // = 10000
        CHECK(ts.duration_stddev_us() == doctest::Approx(100.0));
    }

    TEST_CASE("TraceStatistics - Zero events") {
        TraceStatistics ts;
        ts.success = true;
        ts.num_chunks = 0;

        CHECK(ts.total_events() == 0);
        CHECK(ts.time_span_seconds() == 0.0);
        CHECK(ts.duration_mean_us() == 0.0);
        CHECK(ts.duration_stddev_us() == 0.0);
        CHECK(ts.num_categories() == 0);
        CHECK(ts.num_unique_names() == 0);
        CHECK(ts.num_pid_tids() == 0);
    }

    TEST_CASE("TraceStatistics - to_json produces valid JSON") {
        TraceStatistics ts;
        ts.file_path = "/test/file.pfw.gz";
        ts.index_path = "/test/file.pfw.gz.idx";
        ts.success = true;
        ts.num_chunks = 2;

        ts.merged.update_from_event("read", "POSIX", 1, 1, 1000, 100);
        ts.merged.update_from_event("write", "storage", 2, 2, 2000, 200);

        std::string json = ts.to_json();

        // Parse and validate the JSON
        simdjson::dom::parser parser;
        auto result = parser.parse(json);
        REQUIRE(!result.error());

        auto root = result.value_unsafe();
        REQUIRE(root.is_object());

        CHECK(std::string(root["file_path"].get_string().value()) ==
              "/test/file.pfw.gz");
        CHECK(root["success"].get_bool().value() == true);
        CHECK(root["total_events"].get_uint64().value() == 2);
        CHECK(root["num_chunks"].get_uint64().value() == 2);
        CHECK(root["num_categories"].get_uint64().value() == 2);
        CHECK(root["num_unique_names"].get_uint64().value() == 2);

        // Check time_range object exists
        auto time_range = root["time_range"];
        REQUIRE(!time_range.error());
        REQUIRE(time_range.is_object());

        // Check duration object exists
        auto duration = root["duration"];
        REQUIRE(!duration.error());
        REQUIRE(duration.is_object());
        CHECK(duration["count"].get_uint64().value() == 2);

        // Check category_counts object exists
        auto cats = root["category_counts"];
        REQUIRE(!cats.error());
        REQUIRE(cats.is_object());
    }

    TEST_CASE("TraceStatistics - to_json with error") {
        TraceStatistics ts;
        ts.file_path = "/test/missing.pfw.gz";
        ts.index_path = "/test/missing.pfw.gz.idx";
        ts.success = false;
        ts.error_message = "File not found";

        std::string json = ts.to_json();

        simdjson::dom::parser parser;
        auto result = parser.parse(json);
        REQUIRE(!result.error());

        auto root = result.value_unsafe();
        CHECK(root["success"].get_bool().value() == false);
        CHECK(std::string(root["error"].get_string().value()) ==
              "File not found");
    }
}
