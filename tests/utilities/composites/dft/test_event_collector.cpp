#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/event_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <fstream>

using namespace dftracer::utils::utilities::composites::dft;
using namespace dft_utils_test;

TEST_SUITE("EventCollector") {
    TEST_CASE("EventCollector - Collect from metadata") {
        // Create test environment
        TestEnvironment env(100);

        // Create a DFTracer test file
        std::string test_file = env.create_dft_test_file(10);

        // First collect metadata
        auto meta_input = MetadataCollectorUtilityInput::from_file(test_file);
        MetadataCollectorUtility meta_collector;
        auto meta_output = meta_collector.process(meta_input);

        // Now collect events from metadata
        std::vector<MetadataCollectorUtilityOutput> metadata_vec = {
            meta_output};
        auto input =
            EventCollectorFromMetadataCollectorUtilityInput::from_metadata(
                metadata_vec);

        EventCollectorFromMetadataUtility collector;
        auto event_ids = collector.process(input);

        // Verify we got events
        CHECK(event_ids.size() > 0);
        if (event_ids.size() > 0) {
            // Check first event
            CHECK(event_ids[0].id > 0);
        }
    }

    TEST_CASE("EventCollector - Collect from chunks") {
        // Create mock chunk output
        std::vector<ChunkExtractorUtilityOutput> chunks;

        ChunkExtractorUtilityOutput chunk1;
        chunk1.chunk_index = 0;
        chunk1.success = true;
        chunk1.events = 5;
        // Add some mock event IDs
        chunk1.event_ids.push_back(EventId(1, 1000, 2000));
        chunk1.event_ids.push_back(EventId(2, 1001, 2001));
        chunks.push_back(chunk1);

        auto input = EventCollectorFromChunksUtilityInput::from_chunks(chunks)
                         .with_checkpoint_size(10);

        EventCollectorFromChunksUtility collector;
        auto event_ids = collector.process(input);

        // Should get the event IDs from the chunks
        CHECK(event_ids.size() >= 2);
    }
}
