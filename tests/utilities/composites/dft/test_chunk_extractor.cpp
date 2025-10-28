#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/chunk_extractor.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <fstream>

using namespace dftracer::utils::utilities::composites::dft;
using namespace dft_utils_test;

TEST_SUITE("ChunkExtractor") {
    TEST_CASE("ChunkExtractor - Basic extraction") {
        // Create test environment
        TestEnvironment env(100);

        // Create a DFTracer test file
        std::string test_file = env.create_dft_test_file(5);

        // Create manifest
        DFTracerChunkManifest manifest;
        manifest.total_size_mb = 0.001;

        DFTracerChunkSpec spec;
        spec.file_path = test_file;
        spec.size_mb = 0.001;
        spec.start_line = 0;
        spec.end_line = 4;
        spec.start_byte = 0;
        spec.end_byte = fs::file_size(test_file);
        manifest.specs.push_back(spec);

        // Create input
        auto input = ChunkExtractorUtilityInput::from_manifest(0, manifest)
                         .with_output_dir(env.get_dir())
                         .with_app_name("test");

        // Process
        ChunkExtractorUtility extractor;
        auto output = extractor.process(input);

        // Verify basic properties
        CHECK(output.chunk_index == 0);
        CHECK(output.success == true);
        CHECK(output.events > 0);
    }
}
