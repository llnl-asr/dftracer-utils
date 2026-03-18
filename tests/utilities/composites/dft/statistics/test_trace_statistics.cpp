#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/statistics/trace_statistics.h>
#include <doctest/doctest.h>
#include <yyjson.h>

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
        ts.idx_path = "/test/file.pfw.gz.idx";
        ts.success = true;
        ts.num_chunks = 2;

        ts.merged.update_from_event("read", "POSIX", 1, 1, 1000, 100);
        ts.merged.update_from_event("write", "storage", 2, 2, 2000, 200);

        std::string json = ts.to_json();

        // Parse and validate the JSON
        yyjson_doc* doc =
            yyjson_read(json.c_str(), json.size(), YYJSON_READ_NOFLAG);
        REQUIRE(doc != nullptr);

        yyjson_val* root = yyjson_doc_get_root(doc);
        REQUIRE(yyjson_is_obj(root));

        CHECK(std::string(yyjson_get_str(yyjson_obj_get(root, "file_path"))) ==
              "/test/file.pfw.gz");
        CHECK(yyjson_get_bool(yyjson_obj_get(root, "success")) == true);
        CHECK(yyjson_get_uint(yyjson_obj_get(root, "total_events")) == 2);
        CHECK(yyjson_get_uint(yyjson_obj_get(root, "num_chunks")) == 2);
        CHECK(yyjson_get_uint(yyjson_obj_get(root, "num_categories")) == 2);
        CHECK(yyjson_get_uint(yyjson_obj_get(root, "num_unique_names")) == 2);

        // Check time_range object exists
        yyjson_val* time_range = yyjson_obj_get(root, "time_range");
        REQUIRE(yyjson_is_obj(time_range));

        // Check duration object exists
        yyjson_val* duration = yyjson_obj_get(root, "duration");
        REQUIRE(yyjson_is_obj(duration));
        CHECK(yyjson_get_uint(yyjson_obj_get(duration, "count")) == 2);

        // Check category_counts object exists
        yyjson_val* cats = yyjson_obj_get(root, "category_counts");
        REQUIRE(yyjson_is_obj(cats));

        yyjson_doc_free(doc);
    }

    TEST_CASE("TraceStatistics - to_json with error") {
        TraceStatistics ts;
        ts.file_path = "/test/missing.pfw.gz";
        ts.idx_path = "/test/missing.pfw.gz.idx";
        ts.success = false;
        ts.error_message = "File not found";

        std::string json = ts.to_json();

        yyjson_doc* doc =
            yyjson_read(json.c_str(), json.size(), YYJSON_READ_NOFLAG);
        REQUIRE(doc != nullptr);

        yyjson_val* root = yyjson_doc_get_root(doc);
        CHECK(yyjson_get_bool(yyjson_obj_get(root, "success")) == false);
        CHECK(std::string(yyjson_get_str(yyjson_obj_get(root, "error"))) ==
              "File not found");

        yyjson_doc_free(doc);
    }
}
