#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using dftracer::utils::utilities::indexer::IndexBuildConfig;
using dftracer::utils::utilities::indexer::IndexBuilderUtility;
using dftracer::utils::utilities::indexer::IndexBuildResult;
using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::internal::get_logical_path;
namespace queries =
    dftracer::utils::utilities::composites::dft::indexing::queries;

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

        auto pipeline_config = PipelineConfig()
                                   .with_name("ManifestBuilderTest")
                                   .with_compute_threads(2)
                                   .with_watchdog(false);

        Pipeline pipeline(pipeline_config);
        IndexBuildResult result;

        auto* result_ptr = &result;
        auto task = make_task(
            [trace_file, test_dir,
             result_ptr](CoroScope&) -> coro::CoroTask<void> {
                IndexBuilderUtility builder;
                auto config = IndexBuildConfig::for_file(trace_file)
                                  .with_index_dir(test_dir)
                                  .with_manifest(true)
                                  .with_index_threshold(0);
                *result_ptr = co_await builder.process(config);
                co_return;
            },
            "BuildManifest");

        pipeline.set_source(task);
        pipeline.set_destination(task);
        pipeline.execute();

        CHECK(result.success == true);
        CHECK(result.total_lines > 0);
        // chunks_processed may be 0 for small files (single chunk)
        // events_processed is only populated by bloom visitor

        // Verify .idx file exists
        CHECK(fs::exists(result.idx_path));

        // Query the .idx and verify contents
        IndexDatabase idx_db(result.idx_path);
        idx_db.init_base_schema();
        idx_db.init_manifest_schema();
        int fid = idx_db.get_file_info_id(get_logical_path(trace_file));
        REQUIRE(fid >= 0);

        auto event_ranges = queries::query_event_ranges(idx_db.sql_db(), fid);
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

        auto metadata = queries::query_metadata_lines(idx_db.sql_db(), fid);
        CHECK(metadata.size() == 2);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Skip already-indexed file") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_manifest_skip")
                .string();
        fs::create_directories(test_dir);

        std::string trace_file = create_test_trace(test_dir);

        auto pipeline_config = PipelineConfig()
                                   .with_name("ManifestSkipTest")
                                   .with_compute_threads(2)
                                   .with_watchdog(false);

        // First build
        {
            Pipeline pipeline(pipeline_config);
            IndexBuildResult result;
            auto* result_ptr = &result;
            auto task = make_task(
                [trace_file, test_dir,
                 result_ptr](CoroScope&) -> coro::CoroTask<void> {
                    IndexBuilderUtility builder;
                    auto config = IndexBuildConfig::for_file(trace_file)
                                      .with_index_dir(test_dir)
                                      .with_manifest(true)
                                      .with_index_threshold(0)
                                      .with_force_rebuild(false);
                    *result_ptr = co_await builder.process(config);
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
            IndexBuildResult result;
            auto* result_ptr = &result;
            auto task = make_task(
                [trace_file, test_dir,
                 result_ptr](CoroScope&) -> coro::CoroTask<void> {
                    IndexBuilderUtility builder;
                    auto config = IndexBuildConfig::for_file(trace_file)
                                      .with_index_dir(test_dir)
                                      .with_manifest(true)
                                      .with_index_threshold(0)
                                      .with_force_rebuild(false);
                    *result_ptr = co_await builder.process(config);
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
