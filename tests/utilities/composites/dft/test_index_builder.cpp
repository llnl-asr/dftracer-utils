#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <fstream>

using namespace dftracer::utils::utilities::indexer;
using namespace dft_utils_test;

TEST_SUITE("IndexBuilder") {
    TEST_CASE("IndexBuilder - Build index for compressed trace file") {
        // Create test environment
        TestEnvironment env(100);  // 100 lines for test

        SUBCASE("Build index for gzip file") {
            // Create a compressed DFTracer trace file using TestEnvironment
            std::string gz_file = env.create_dft_test_gzip_file(50);
            std::string idx_path = gz_file + ".idx";

            // Create input (threshold=0 so small test files are always indexed)
            auto input = IndexBuildConfig::for_file(gz_file)
                             .with_index_dir("")
                             .with_checkpoint_size(10)
                             .with_index_threshold(0);

            // Build index
            IndexBuilderUtility builder;
            auto output = builder.process(input).get();

            // Verify
            CHECK(output.file_path == gz_file);
            CHECK(output.idx_path == idx_path);
            CHECK(output.success == true);
            CHECK(output.was_skipped == false);

            // Verify index file was created
            CHECK(fs::exists(idx_path));
        }

        SUBCASE("Use existing index without force rebuild") {
            std::string gz_file = env.create_dft_test_gzip_file(20);
            std::string idx_path = gz_file + ".idx";

            // Build index first time
            auto input1 = IndexBuildConfig::for_file(gz_file)
                              .with_index_dir("")
                              .with_index_threshold(0);

            IndexBuilderUtility builder;
            auto output1 = builder.process(input1).get();
            CHECK(output1.success == true);
            CHECK(output1.was_skipped == false);

            // Build again without force - should use existing
            auto output2 = builder.process(input1).get();
            CHECK(output2.success == true);
            CHECK(output2.was_skipped == true);  // Should not rebuild
        }
    }

    TEST_CASE("IndexBuilder - Force rebuild") {
        // Create test environment
        TestEnvironment env(100);

        // Create a compressed DFTracer trace file
        std::string gz_file = env.create_dft_test_gzip_file(30);
        std::string idx_path = gz_file + ".idx";

        auto input = IndexBuildConfig::for_file(gz_file)
                         .with_index_dir("")
                         .with_force_rebuild(true)
                         .with_index_threshold(0);

        IndexBuilderUtility builder;
        auto output1 = builder.process(input).get();
        CHECK(output1.success == true);
        CHECK(output1.was_skipped == false);

        // Build again with force
        auto output2 = builder.process(input).get();
        CHECK(output2.success == true);
        CHECK(output2.was_skipped == false);  // Should rebuild with force
    }

    TEST_CASE("IndexBuilder - Non-existent file") {
        auto input = IndexBuildConfig::for_file("/non/existent/file.gz");

        IndexBuilderUtility builder;
        auto output = builder.process(input).get();

        CHECK(output.success == false);
        CHECK(output.index_created == false);
    }
}
