// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/event_collector_utility.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <doctest/doctest.h>
#include <refused_lines.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <unistd.h>

using namespace dftracer::utils;

using namespace dftracer::utils::trace;
using namespace dftu_utils_test;

TEST_SUITE("EventCollector") {
    TEST_CASE("EventCollector - Collect from metadata") {
        // Create test environment
        TestEnvironment env(100);

        // Create a DFTracer test file
        dstd::String test_file = env.create_dft_test_gzip_file(10);

        // First collect metadata
        auto meta_input =
            MetadataCollectorUtilityInput::from_file(dstd::String(test_file))
                .with_compute_hash(true);
        MetadataCollectorUtility meta_collector;
        auto meta_output =
            dftu_utils_test::ok(meta_collector(meta_input).get());

        // Now collect events from metadata
        dstd::Vector<MetadataCollectorUtilityOutput> metadata_vec = {
            meta_output};
        auto input =
            EventCollectorFromMetadataCollectorUtilityInput::from_metadata(
                metadata_vec);

        EventCollectorFromMetadataUtility collector;
        auto event_ids = dftu_utils_test::ok(collector(input).get());

        // Verify we got events
        CHECK(event_ids.size() > 0);
        if (event_ids.size() > 0) {
            // Check first event
            CHECK(event_ids[0].id > 0);
        }
    }

    TEST_CASE("EventCollector - a line the DOM parser refuses gives its id") {
        TestEnvironment env(100);
        const dstd::String pfw = env.dir() + "/refused.pfw";
        {
            FileOut ofs(pfw);
            for (int i = 0; i < 5; ++i)
                ofs << R"({"id":)" << i + 1
                    << R"(,"ph":"X","name":"read","cat":"POSIX","pid":1,)"
                    << R"("tid":1,"ts":)" << 1000 + i
                    << R"(,"dur":10,"args":{"ret":4096}})" << "\n";
            for (int k = 0; k < REFUSED_KINDS; ++k)
                ofs << refused_line(
                           k, R"("id":)" + dstd::to_string(100 + k) +
                                  R"(,"ph":"X","name":"odd","cat":"POSIX",)"
                                  R"("pid":2,"tid":3,"ts":2000,"dur":10)")
                    << "\n";
        }
        const dstd::String gz = pfw + ".gz";
        REQUIRE(compress_file_to_gzip(pfw, gz));

        auto meta_output = dftu_utils_test::ok(
            MetadataCollectorUtility()(
                MetadataCollectorUtilityInput::from_file(dstd::String(gz))
                    .with_compute_hash(true))
                .get());
        dstd::Vector<MetadataCollectorUtilityOutput> metadata_vec = {
            meta_output};
        auto ids = dftu_utils_test::ok(
            EventCollectorFromMetadataUtility()(
                EventCollectorFromMetadataCollectorUtilityInput::from_metadata(
                    metadata_vec))
                .get());

        REQUIRE(ids.size() == 5 + REFUSED_KINDS);
        for (int i = 0; i < 5; ++i) {
            CHECK(ids[static_cast<std::size_t>(i)].id == i + 1);
            CHECK(ids[static_cast<std::size_t>(i)].pid == 1);
        }
        for (int k = 0; k < REFUSED_KINDS; ++k) {
            const auto& e = ids[static_cast<std::size_t>(5 + k)];
            CHECK(e.id == 100 + k);
            CHECK(e.pid == 2);
            CHECK(e.tid == 3);
        }
    }
}
