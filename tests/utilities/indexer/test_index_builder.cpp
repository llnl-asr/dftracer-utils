#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/utilities/behaviors/behavior_chain.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/visitors/bloom_visitor.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::indexer;
using namespace dftracer::utils::utilities::behaviors;
using namespace dft_utils_test;

namespace tags = dftracer::utils::utilities::tags;

namespace {

// Run an IndexBuilderUtility synchronously via Runtime + run_coro_scope.
// The lambda receives (CoroScope&) -> coro::CoroTask<void>.
template <typename Fn>
void run_coro(Fn&& fn) {
    Runtime rt(4);
    auto task = run_coro_scope(rt.executor(), std::forward<Fn>(fn));
    rt.submit(std::move(task), "test").wait();
    rt.shutdown();
}

}  // namespace

TEST_SUITE("IndexBuilder") {
    TEST_CASE("Build checkpoint-only index") {
        TestEnvironment env(1000);
        std::string gz_file = env.create_dft_test_gzip_file(1000);

        auto config =
            IndexBuildConfig::for_file(gz_file).with_bloom(false).with_manifest(
                false);

        IndexBuildResult result;
        run_coro([&config, &result](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            result = co_await exec.execute_with_context(scope, config);
        });

        CHECK(result.success);
        CHECK_FALSE(result.was_skipped);
        CHECK(fs::exists(result.idx_path));
    }

    TEST_CASE("BloomVisitor direct test") {
        using dftracer::utils::utilities::composites::dft::indexing::
            ChunkIndexerConfig;
        BloomVisitor visitor(ChunkIndexerConfig{},
                             {"name", "cat", "pid", "tid"});
        visitor.begin(0);

        std::string json_line =
            R"({"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":100,"dur":50,"ph":"X"})";
        visitor.on_line(json_line, 0);

        CHECK(visitor.num_chunks() >= 1);
        MESSAGE("BloomVisitor chunks after on_line: ", visitor.num_chunks());

        auto db_path = dft_utils_test::make_unique_test_path("bloom_direct");
        db_path += ".idx";
        {
            IndexDatabase db(db_path.string());
            db.init_base_schema();
            db.init_bloom_schema();
            int fid = db.get_or_create_file_info("test.pfw.gz", 123);
            db.begin_transaction();
            visitor.finalize(db, fid);
            db.commit_transaction();
            CHECK(db.has_bloom_data(fid));
        }
        fs::remove(db_path);
    }

    TEST_CASE("Build with bloom") {
        TestEnvironment env(1000);
        std::string gz_file = env.create_dft_test_gzip_file(1000);

        auto config = IndexBuildConfig::for_file(gz_file)
                          .with_bloom(true)
                          .with_manifest(false)
                          .with_index_threshold(0);

        IndexBuildResult result;
        run_coro([&config, &result](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            result = co_await exec.execute_with_context(scope, config);
        });

        REQUIRE(result.success);
        REQUIRE(fs::exists(result.idx_path));

        IndexDatabase db(result.idx_path);
        int fid =
            db.get_file_info_id(internal::get_logical_path(result.file_path));
        REQUIRE(fid >= 0);
        CHECK(db.has_bloom_data(fid));
    }

    TEST_CASE("Build with manifest") {
        TestEnvironment env(1000);
        std::string gz_file = env.create_dft_test_gzip_file(1000);

        auto config = IndexBuildConfig::for_file(gz_file)
                          .with_bloom(false)
                          .with_manifest(true)
                          .with_index_threshold(0);

        IndexBuildResult result;
        run_coro([&config, &result](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            result = co_await exec.execute_with_context(scope, config);
        });

        REQUIRE(result.success);
        REQUIRE(fs::exists(result.idx_path));

        IndexDatabase db(result.idx_path);
        int fid =
            db.get_file_info_id(internal::get_logical_path(result.file_path));
        REQUIRE(fid >= 0);
        CHECK(db.has_manifest_data(fid));
    }

    TEST_CASE("Build with bloom and manifest") {
        TestEnvironment env(1000);
        std::string gz_file = env.create_dft_test_gzip_file(1000);

        auto config = IndexBuildConfig::for_file(gz_file)
                          .with_bloom(true)
                          .with_manifest(true)
                          .with_index_threshold(0);

        IndexBuildResult result;
        run_coro([&config, &result](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            result = co_await exec.execute_with_context(scope, config);
        });

        REQUIRE(result.success);
        REQUIRE(fs::exists(result.idx_path));

        IndexDatabase db(result.idx_path);
        int fid =
            db.get_file_info_id(internal::get_logical_path(result.file_path));
        REQUIRE(fid >= 0);
        CHECK(db.has_bloom_data(fid));
        CHECK(db.has_manifest_data(fid));
    }

    TEST_CASE("Skip if already indexed") {
        TestEnvironment env(1000);
        std::string gz_file = env.create_dft_test_gzip_file(1000);

        auto config = IndexBuildConfig::for_file(gz_file)
                          .with_bloom(false)
                          .with_manifest(false)
                          .with_force_rebuild(false)
                          .with_index_threshold(0);

        IndexBuildResult first;
        run_coro([&config, &first](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            first = co_await exec.execute_with_context(scope, config);
        });
        REQUIRE(first.success);
        CHECK_FALSE(first.was_skipped);

        IndexBuildResult second;
        run_coro([&config, &second](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            second = co_await exec.execute_with_context(scope, config);
        });
        CHECK(second.success);
        CHECK(second.was_skipped);
    }

    TEST_CASE("Force rebuild") {
        TestEnvironment env(1000);
        std::string gz_file = env.create_dft_test_gzip_file(1000);

        auto config_normal = IndexBuildConfig::for_file(gz_file)
                                 .with_bloom(false)
                                 .with_manifest(false)
                                 .with_force_rebuild(false)
                                 .with_index_threshold(0);

        IndexBuildResult first;
        run_coro([&config_normal,
                  &first](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            first = co_await exec.execute_with_context(scope, config_normal);
        });
        REQUIRE(first.success);

        auto config_force = IndexBuildConfig::for_file(gz_file)
                                .with_bloom(false)
                                .with_manifest(false)
                                .with_force_rebuild(true)
                                .with_index_threshold(0);

        IndexBuildResult second;
        run_coro([&config_force,
                  &second](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            second = co_await exec.execute_with_context(scope, config_force);
        });
        CHECK(second.success);
        CHECK_FALSE(second.was_skipped);
    }

    TEST_CASE("Result has correct line count") {
        TestEnvironment env(1000);
        std::string gz_file = env.create_dft_test_gzip_file(1000);

        auto config =
            IndexBuildConfig::for_file(gz_file).with_bloom(false).with_manifest(
                false);

        IndexBuildResult result;
        run_coro([&config, &result](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            result = co_await exec.execute_with_context(scope, config);
        });

        REQUIRE(result.success);
        CHECK(result.total_lines > 0);
        CHECK(result.total_lines >= 1000);
    }

    TEST_CASE("Incremental bloom add to existing checkpoint-only index") {
        TestEnvironment env(1000);
        std::string gz_file = env.create_dft_test_gzip_file(1000);

        // First build: checkpoint only
        auto config1 = IndexBuildConfig::for_file(gz_file)
                           .with_bloom(false)
                           .with_manifest(false)
                           .with_index_threshold(0);

        IndexBuildResult r1;
        run_coro([&config1, &r1](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            r1 = co_await exec.execute_with_context(scope, config1);
        });
        REQUIRE(r1.success);
        CHECK(r1.index_created);

        // Verify no bloom data yet
        {
            IndexDatabase db(r1.idx_path);
            int fid = db.get_file_info_id(internal::get_logical_path(gz_file));
            CHECK(fid >= 0);
            CHECK_FALSE(db.has_bloom_data(fid));
        }

        // Second build: add bloom (should NOT rebuild checkpoints)
        auto config2 = IndexBuildConfig::for_file(gz_file)
                           .with_bloom(true)
                           .with_manifest(false)
                           .with_index_threshold(0);

        IndexBuildResult r2;
        run_coro([&config2, &r2](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            r2 = co_await exec.execute_with_context(scope, config2);
        });
        REQUIRE(r2.success);
        CHECK_FALSE(r2.was_skipped);

        // Verify bloom data now exists
        {
            IndexDatabase db(r2.idx_path);
            int fid = db.get_file_info_id(internal::get_logical_path(gz_file));
            CHECK(fid >= 0);
            CHECK(db.has_bloom_data(fid));
        }
    }

    TEST_CASE("Incremental manifest add to existing index with bloom") {
        TestEnvironment env(1000);
        std::string gz_file = env.create_dft_test_gzip_file(1000);

        // First build: checkpoint + bloom
        auto config1 = IndexBuildConfig::for_file(gz_file)
                           .with_bloom(true)
                           .with_manifest(false)
                           .with_index_threshold(0);

        IndexBuildResult r1;
        run_coro([&config1, &r1](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            r1 = co_await exec.execute_with_context(scope, config1);
        });
        REQUIRE(r1.success);

        {
            IndexDatabase db(r1.idx_path);
            int fid = db.get_file_info_id(internal::get_logical_path(gz_file));
            CHECK(db.has_bloom_data(fid));
            CHECK_FALSE(db.has_manifest_data(fid));
        }

        // Second build: add manifest (bloom already exists, skip it)
        auto config2 = IndexBuildConfig::for_file(gz_file)
                           .with_bloom(false)
                           .with_manifest(true)
                           .with_index_threshold(0);

        IndexBuildResult r2;
        run_coro([&config2, &r2](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            r2 = co_await exec.execute_with_context(scope, config2);
        });
        REQUIRE(r2.success);
        CHECK_FALSE(r2.was_skipped);

        // Verify both bloom and manifest exist
        {
            IndexDatabase db(r2.idx_path);
            int fid = db.get_file_info_id(internal::get_logical_path(gz_file));
            CHECK(db.has_bloom_data(fid));
            CHECK(db.has_manifest_data(fid));
        }
    }

    TEST_CASE("Skip when all requested features already exist") {
        TestEnvironment env(1000);
        std::string gz_file = env.create_dft_test_gzip_file(1000);

        // Build with bloom + manifest
        auto config1 = IndexBuildConfig::for_file(gz_file)
                           .with_bloom(true)
                           .with_manifest(true)
                           .with_index_threshold(0);

        IndexBuildResult r1;
        run_coro([&config1, &r1](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            r1 = co_await exec.execute_with_context(scope, config1);
        });
        REQUIRE(r1.success);
        CHECK_FALSE(r1.was_skipped);

        // Build again with same features — should skip
        IndexBuildResult r2;
        run_coro([&config1, &r2](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder,
                     BehaviorChain<IndexBuildConfig, IndexBuildResult>{});
            r2 = co_await exec.execute_with_context(scope, config1);
        });
        REQUIRE(r2.success);
        CHECK(r2.was_skipped);
    }
}
