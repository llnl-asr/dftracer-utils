// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/event_collector_utility.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <doctest/doctest.h>
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
        auto meta_output = meta_collector(meta_input).get();

        // Now collect events from metadata
        dstd::Vector<MetadataCollectorUtilityOutput> metadata_vec = {
            meta_output};
        auto input =
            EventCollectorFromMetadataCollectorUtilityInput::from_metadata(
                metadata_vec);

        EventCollectorFromMetadataUtility collector;
        auto event_ids = collector(input).get();

        // Verify we got events
        CHECK(event_ids.size() > 0);
        if (event_ids.size() > 0) {
            // Check first event
            CHECK(event_ids[0].id > 0);
        }
    }
}
