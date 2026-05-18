#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/utilities/behaviors/behavior_chain.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
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
using dftracer::utils::utilities::behaviors::BehaviorChain;
using dftracer::utils::utilities::behaviors::UtilityExecutor;
using dftracer::utils::utilities::indexer::IndexBuildConfig;
using dftracer::utils::utilities::indexer::IndexBuilderUtility;
using dftracer::utils::utilities::indexer::IndexBuildResult;
using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::internal::get_logical_path;
namespace queries =
    dftracer::utils::utilities::composites::dft::indexing::queries;
namespace tags = dftracer::utils::utilities::tags;

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

// Helper: run IndexBuilderUtility via Runtime + run_coro_scope.
static IndexBuildResult run_index_build(const IndexBuildConfig& config) {
    Runtime rt(4);
    IndexBuildResult result;
    auto* result_ptr = &result;

    auto task = run_coro_scope(
        rt.executor(),
        [config, result_ptr](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            *result_ptr = co_await exec.execute_with_context(scope, config);
        });

    rt.submit(std::move(task), "index-build").wait();
    rt.shutdown();
    return result;
}

TEST_SUITE("ManifestIndexBuilder") {
    TEST_CASE("Build manifest index and query results") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_manifest_builder")
                .string();
        fs::create_directories(test_dir);

        std::string trace_file = create_test_trace(test_dir);

        auto config = IndexBuildConfig::for_file(trace_file)
                          .with_index_dir(test_dir)
                          .with_manifest(true);

        auto result = run_index_build(config);

        CHECK(result.success == true);
        CHECK(result.total_lines > 0);

        CHECK(fs::exists(result.index_path));

        IndexDatabase idx_db(result.index_path);
        idx_db.init_schema();
        int fid = idx_db.get_file_info_id(get_logical_path(trace_file));
        REQUIRE(fid >= 0);

        auto event_ranges = idx_db.query_event_ranges(fid);
        CHECK(event_ranges.size() == 3);

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

        auto metadata = idx_db.query_metadata_lines(fid);
        CHECK(metadata.size() == 2);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Skip already-indexed file") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_manifest_skip")
                .string();
        fs::create_directories(test_dir);

        std::string trace_file = create_test_trace(test_dir);

        // First build
        {
            auto config = IndexBuildConfig::for_file(trace_file)
                              .with_index_dir(test_dir)
                              .with_manifest(true)
                              .with_force_rebuild(false);

            auto result = run_index_build(config);
            CHECK(result.success == true);
            CHECK(result.was_skipped == false);
        }

        // Second build should skip
        {
            auto config = IndexBuildConfig::for_file(trace_file)
                              .with_index_dir(test_dir)
                              .with_manifest(true)
                              .with_force_rebuild(false);

            auto result = run_index_build(config);
            CHECK(result.success == true);
            CHECK(result.was_skipped == true);
        }

        fs::remove_all(test_dir);
    }
}
