#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <fstream>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::indexer;
using namespace dftracer::utils::utilities::behaviors;
using namespace dftracer::utils::utilities::composites::dft::internal;
using namespace dft_utils_test;

namespace tags = dftracer::utils::utilities::tags;

// Helper: run IndexBuilderUtility via Runtime + run_coro_scope.
static IndexBuildResult run_builder(const IndexBuildConfig& config) {
    Runtime rt(4);
    IndexBuildResult result;
    auto* result_ptr = &result;

    auto task = run_coro_scope(
        rt.executor(),
        [config, result_ptr](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder);
            *result_ptr = co_await exec.execute(scope, config);
        });

    rt.submit(std::move(task), "index-build").wait();
    rt.shutdown();
    return result;
}

TEST_SUITE("IndexBuilder") {
    TEST_CASE("IndexBuilder - Build index for compressed trace file") {
        TestEnvironment env(100);

        SUBCASE("Build index for gzip file") {
            std::string gz_file = env.create_dft_test_gzip_file(50);
            std::string db_root = determine_index_path(gz_file, "");

            auto input = IndexBuildConfig::for_file(gz_file)
                             .with_index_dir("")
                             .with_checkpoint_size(10);

            auto output = run_builder(input);

            CHECK(output.file_path == gz_file);
            CHECK(output.index_path == db_root);
            CHECK(output.success == true);
            CHECK(output.was_skipped == false);

            CHECK(fs::exists(db_root));
        }

        SUBCASE("Use existing index without force rebuild") {
            std::string gz_file = env.create_dft_test_gzip_file(20);

            auto input1 =
                IndexBuildConfig::for_file(gz_file).with_index_dir("");

            auto output1 = run_builder(input1);
            CHECK(output1.success == true);
            CHECK(output1.was_skipped == false);

            auto output2 = run_builder(input1);
            CHECK(output2.success == true);
            CHECK(output2.was_skipped == true);
        }
    }

    TEST_CASE("IndexBuilder - Force rebuild") {
        TestEnvironment env(100);

        std::string gz_file = env.create_dft_test_gzip_file(30);

        auto input = IndexBuildConfig::for_file(gz_file)
                         .with_index_dir("")
                         .with_force_rebuild(true);

        auto output1 = run_builder(input);
        CHECK(output1.success == true);
        CHECK(output1.was_skipped == false);

        auto output2 = run_builder(input);
        CHECK(output2.success == true);
        CHECK(output2.was_skipped == false);
    }

    TEST_CASE("IndexBuilder - Non-existent file") {
        auto input = IndexBuildConfig::for_file("/non/existent/file.gz");

        auto output = run_builder(input);

        CHECK(output.success == false);
        CHECK(output.index_created == false);
    }
}
