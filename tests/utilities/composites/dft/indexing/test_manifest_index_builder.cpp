#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/behaviors/behavior_chain.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/utilities/composites/dft/indexing/manifest_index_builder.h>
#include <dftracer/utils/utilities/composites/dft/indexing/manifest_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft::indexing;

static std::string create_test_trace(const std::string& dir) {
    std::string plain_path = dir + "/test_builder.trace";
    {
        std::ofstream ofs(plain_path);
        ofs << R"({"name":"HH","ph":"M","args":{"value":"h1","name":"host1"}})"
            << "\n";
        ofs << R"({"name":"FH","ph":"M","args":{"value":"f1","name":"/data/a.h5"}})"
            << "\n";
        ofs << R"({"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":100,"ph":"X","args":{"hhash":"h1","fhash":"f1"}})"
            << "\n";
        ofs << R"({"name":"write","cat":"POSIX","pid":1,"tid":1,"ts":2000,"dur":200,"ph":"X","args":{"hhash":"h1","fhash":"f1"}})"
            << "\n";
        ofs << R"({"name":"compute","cat":"APP","pid":1,"tid":1,"ts":3000,"dur":500,"ph":"X","args":{"hhash":"h1"}})"
            << "\n";
        ofs << R"({"name":"read","cat":"POSIX","pid":2,"tid":1,"ts":4000,"dur":120,"ph":"X","args":{"hhash":"h1","fhash":"f1"}})"
            << "\n";
        ofs.close();
    }

    std::string gz_path = plain_path + ".gz";
    dft_utils_test::compress_file_to_gzip(plain_path, gz_path);
    fs::remove(plain_path);
    return gz_path;
}

TEST_SUITE("ManifestIndexBuilder") {
    TEST_CASE("Build manifest index and query results") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_manifest_builder")
                .string();
        fs::create_directories(test_dir);

        std::string trace_file = create_test_trace(test_dir);

        ManifestIndexBuildInput input;
        input.file_path = trace_file;
        input.index_dir = test_dir;
        input.force_rebuild = true;

        auto pipeline_config = PipelineConfig()
                                   .with_name("ManifestBuilderTest")
                                   .with_compute_threads(2)
                                   .with_watchdog(false);

        Pipeline pipeline(pipeline_config);

        ManifestIndexBuildOutput result;

        auto task = make_task(
            [&](CoroScope& ctx) -> coro::CoroTask<void> {
                auto utility = std::make_shared<ManifestIndexBuilderUtility>();
                behaviors::BehaviorChain<ManifestIndexBuildInput,
                                         ManifestIndexBuildOutput>
                    chain;
                behaviors::UtilityExecutor<ManifestIndexBuildInput,
                                           ManifestIndexBuildOutput,
                                           utilities::tags::NeedsContext>
                    executor(utility, std::move(chain));

                result = co_await executor.execute_with_context(ctx, input);
                co_return;
            },
            "BuildManifest");

        pipeline.set_source(task);
        pipeline.set_destination(task);
        pipeline.execute();

        CHECK(result.success == true);
        CHECK(result.events_processed == 4);
        CHECK(result.chunks_processed == 1);

        // Verify .midx file exists
        std::string midx_path =
            determine_manifest_index_path(trace_file, test_dir);
        CHECK(fs::exists(midx_path));

        // Query the .midx and verify contents
        ManifestIndexDatabase midx(midx_path);
        midx.init_schema();
        int fid = midx.get_file_info_id(trace_file);
        REQUIRE(fid >= 0);

        auto event_ranges = queries::query_event_ranges(midx.db(), fid);
        CHECK(event_ranges.size() == 3);

        // Find POSIX/read group
        bool found_posix_read = false;
        for (const auto& r : event_ranges) {
            if (r.cat == "POSIX" && r.name == "read") {
                found_posix_read = true;
                CHECK(r.event_count == 2);
                CHECK(r.line_numbers.size() == 2);
                CHECK(r.line_numbers[0] == 2);
                CHECK(r.line_numbers[1] == 5);
            }
        }
        CHECK(found_posix_read);

        auto metadata = queries::query_metadata_lines(midx.db(), fid);
        CHECK(metadata.size() == 2);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Skip already-indexed file") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_manifest_skip")
                .string();
        fs::create_directories(test_dir);

        std::string trace_file = create_test_trace(test_dir);

        ManifestIndexBuildInput input;
        input.file_path = trace_file;
        input.index_dir = test_dir;
        input.force_rebuild = false;

        auto pipeline_config = PipelineConfig()
                                   .with_name("ManifestSkipTest")
                                   .with_compute_threads(2)
                                   .with_watchdog(false);

        // First build
        {
            Pipeline pipeline(pipeline_config);
            ManifestIndexBuildOutput result;
            auto task = make_task(
                [&](CoroScope& ctx) -> coro::CoroTask<void> {
                    auto utility =
                        std::make_shared<ManifestIndexBuilderUtility>();
                    behaviors::BehaviorChain<ManifestIndexBuildInput,
                                             ManifestIndexBuildOutput>
                        chain;
                    behaviors::UtilityExecutor<ManifestIndexBuildInput,
                                               ManifestIndexBuildOutput,
                                               utilities::tags::NeedsContext>
                        executor(utility, std::move(chain));
                    result = co_await executor.execute_with_context(ctx, input);
                    co_return;
                },
                "Build1");
            pipeline.set_source(task);
            pipeline.set_destination(task);
            pipeline.execute();
            CHECK(result.success == true);
            CHECK(result.was_skipped == false);
        }

        // Second build should skip
        {
            Pipeline pipeline(pipeline_config);
            ManifestIndexBuildOutput result;
            auto task = make_task(
                [&](CoroScope& ctx) -> coro::CoroTask<void> {
                    auto utility =
                        std::make_shared<ManifestIndexBuilderUtility>();
                    behaviors::BehaviorChain<ManifestIndexBuildInput,
                                             ManifestIndexBuildOutput>
                        chain;
                    behaviors::UtilityExecutor<ManifestIndexBuildInput,
                                               ManifestIndexBuildOutput,
                                               utilities::tags::NeedsContext>
                        executor(utility, std::move(chain));
                    result = co_await executor.execute_with_context(ctx, input);
                    co_return;
                },
                "Build2");
            pipeline.set_source(task);
            pipeline.set_destination(task);
            pipeline.execute();
            CHECK(result.success == true);
            CHECK(result.was_skipped == true);
        }

        fs::remove_all(test_dir);
    }
}
