#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/index_builder.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <fstream>

using namespace dftracer::utils::utilities::composites::dft;
using namespace dft_utils_test;

TEST_SUITE("IndexBuilder") {
    TEST_CASE("IndexBuilder - Build index for compressed trace file") {
        // Create test environment
        TestEnvironment env(100);  // 100 lines for test

        SUBCASE("Build index for gzip file") {
            // Create a compressed DFTracer trace file using TestEnvironment
            std::string gz_file = env.create_dft_test_gzip_file(50);
            std::string idx_path = gz_file + ".idx";

            // Create input
            auto input = IndexBuildUtilityInput::from_file(gz_file)
                             .with_index(idx_path)
                             .with_checkpoint_size(10);

            // Build index
            IndexBuilderUtility builder;
            auto output = builder.process(input);

            // Verify
            CHECK(output.file_path == gz_file);
            CHECK(output.idx_path == idx_path);
            CHECK(output.success == true);
            CHECK(output.was_built == true);

            // Verify index file was created
            CHECK(fs::exists(idx_path));
        }

        SUBCASE("Use existing index without force rebuild") {
            std::string gz_file = env.create_dft_test_gzip_file(20);
            std::string idx_path = gz_file + ".idx";

            // Build index first time
            auto input1 =
                IndexBuildUtilityInput::from_file(gz_file).with_index(idx_path);

            IndexBuilderUtility builder;
            auto output1 = builder.process(input1);
            CHECK(output1.success == true);
            CHECK(output1.was_built == true);

            // Build again without force - should use existing
            auto output2 = builder.process(input1);
            CHECK(output2.success == true);
            CHECK(output2.was_built == false);  // Should not rebuild
        }
    }

    TEST_CASE("IndexBuilder - Force rebuild") {
        // Create test environment
        TestEnvironment env(100);

        // Create a compressed DFTracer trace file
        std::string gz_file = env.create_dft_test_gzip_file(30);
        std::string idx_path = gz_file + ".idx";

        auto input = IndexBuildUtilityInput::from_file(gz_file)
                         .with_index(idx_path)
                         .with_force_rebuild(true);

        IndexBuilderUtility builder;
        auto output1 = builder.process(input);
        CHECK(output1.success == true);
        CHECK(output1.was_built == true);

        // Build again with force
        auto output2 = builder.process(input);
        CHECK(output2.success == true);
        CHECK(output2.was_built == true);  // Should rebuild with force
    }

    TEST_CASE("IndexBuilder - Non-existent file") {
        auto input = IndexBuildUtilityInput::from_file("/non/existent/file.gz");

        IndexBuilderUtility builder;
        auto output = builder.process(input);

        CHECK(output.success == false);
        CHECK(output.was_built == false);
    }
}
