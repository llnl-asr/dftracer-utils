#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/rocksdb/db_manager.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/utilities/composites/dft/event.h>
#include <dftracer/utils/utilities/composites/dft/visitors/bloom_visitor.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <testing_utilities.h>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::indexer;
using namespace dftracer::utils::utilities::composites::dft::visitors;
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
        TestEnvironment env(valgrind_scale(1000, 10));
        std::string gz_file =
            env.create_dft_test_gzip_file(valgrind_scale(1000, 10));

        auto config = IndexBuildConfig::for_file(gz_file).with_manifest(false);

        IndexBuildResult result;
        run_coro([&config, &result](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder);
            result = co_await exec.execute(scope, config);
        });

        CHECK(result.success);
        CHECK_FALSE(result.was_skipped);
        CHECK(fs::exists(result.index_path));
    }

    TEST_CASE("BloomVisitor direct test") {
        using dftracer::utils::utilities::composites::dft::indexing::
            ChunkIndexerConfig;
        BloomVisitor visitor(ChunkIndexerConfig{},
                             {"name", "cat", "pid", "tid"});
        visitor.begin(0);

        std::string json_line =
            R"({"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":100,"dur":50,"ph":"X"})";
        simdjson::dom::parser parser;
        auto result = parser.parse(json_line.data(), json_line.size());
        REQUIRE(!result.error());
        dftracer::utils::utilities::common::json::JsonValue json(
            result.value_unsafe());
        dftracer::utils::utilities::composites::dft::DFTracerEvent ev;
        REQUIRE(decltype(ev)::parse(json, ev));
        dftracer::utils::utilities::composites::dft::EventRecord record{
            ev, json, json_line, 0, 0, 0};
        visitor.on_event(record);

        CHECK(visitor.num_chunks() >= 1);
        MESSAGE("BloomVisitor chunks after on_event: ", visitor.num_chunks());

        auto db_path = dft_utils_test::make_unique_test_path("bloom_direct");
        db_path /= ".dftindex";
        fs::remove_all(db_path);
        dftracer::utils::rocksdb::RocksDBManager::instance().reset(
            db_path.string());
        {
            IndexDatabase db(db_path.string());
            db.init_schema();
            auto writer = db.begin_write();
            int fid = writer->get_or_create_file_info("test.pfw.gz", 123);
            visitor.finalize(*writer, fid);
            writer->commit();
            CHECK(db.has_bloom_data(fid));
        }
        fs::remove_all(db_path);
    }

    TEST_CASE("Build with bloom") {
        TestEnvironment env(valgrind_scale(1000, 10));
        std::string gz_file =
            env.create_dft_test_gzip_file(valgrind_scale(1000, 10));

        auto config = IndexBuildConfig::for_file(gz_file)

                          .with_manifest(false);

        IndexBuildResult result;
        run_coro([&config, &result](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder);
            result = co_await exec.execute(scope, config);
        });

        REQUIRE(result.success);
        REQUIRE(fs::exists(result.index_path));

        IndexDatabase db(result.index_path);
        int fid =
            db.get_file_info_id(internal::get_logical_path(result.file_path));
        REQUIRE(fid >= 0);
        CHECK(db.has_bloom_data(fid));
    }

    TEST_CASE("Build with manifest") {
        TestEnvironment env(valgrind_scale(1000, 10));
        std::string gz_file =
            env.create_dft_test_gzip_file(valgrind_scale(1000, 10));

        auto config = IndexBuildConfig::for_file(gz_file)

                          .with_manifest(true);

        IndexBuildResult result;
        run_coro([&config, &result](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder);
            result = co_await exec.execute(scope, config);
        });

        REQUIRE(result.success);
        REQUIRE(fs::exists(result.index_path));

        IndexDatabase db(result.index_path);
        int fid =
            db.get_file_info_id(internal::get_logical_path(result.file_path));
        REQUIRE(fid >= 0);
        CHECK(db.has_manifest_data(fid));
    }

    TEST_CASE("Build with bloom and manifest") {
        TestEnvironment env(valgrind_scale(1000, 10));
        std::string gz_file =
            env.create_dft_test_gzip_file(valgrind_scale(1000, 10));

        auto config = IndexBuildConfig::for_file(gz_file)

                          .with_manifest(true);

        IndexBuildResult result;
        run_coro([&config, &result](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder);
            result = co_await exec.execute(scope, config);
        });

        REQUIRE(result.success);
        REQUIRE(fs::exists(result.index_path));

        IndexDatabase db(result.index_path);
        int fid =
            db.get_file_info_id(internal::get_logical_path(result.file_path));
        REQUIRE(fid >= 0);
        CHECK(db.has_bloom_data(fid));
        CHECK(db.has_manifest_data(fid));
    }

    TEST_CASE("Skip if already indexed") {
        TestEnvironment env(valgrind_scale(1000, 10));
        std::string gz_file =
            env.create_dft_test_gzip_file(valgrind_scale(1000, 10));

        auto config = IndexBuildConfig::for_file(gz_file)

                          .with_manifest(false)
                          .with_force_rebuild(false);

        IndexBuildResult first;
        run_coro([&config, &first](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder);
            first = co_await exec.execute(scope, config);
        });
        REQUIRE(first.success);
        CHECK_FALSE(first.was_skipped);

        IndexBuildResult second;
        run_coro([&config, &second](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder);
            second = co_await exec.execute(scope, config);
        });
        CHECK(second.success);
        CHECK(second.was_skipped);
    }

    TEST_CASE("Force rebuild") {
        TestEnvironment env(valgrind_scale(1000, 10));
        std::string gz_file =
            env.create_dft_test_gzip_file(valgrind_scale(1000, 10));

        auto config_normal = IndexBuildConfig::for_file(gz_file)

                                 .with_manifest(false)
                                 .with_force_rebuild(false);

        IndexBuildResult first;
        run_coro(
            [&config_normal, &first](CoroScope& scope) -> coro::CoroTask<void> {
                auto builder = std::make_shared<IndexBuilderUtility>();
                UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                                tags::NeedsContext>
                    exec(builder);
                first = co_await exec.execute(scope, config_normal);
            });
        REQUIRE(first.success);

        auto config_force = IndexBuildConfig::for_file(gz_file)

                                .with_manifest(false)
                                .with_force_rebuild(true);

        IndexBuildResult second;
        run_coro(
            [&config_force, &second](CoroScope& scope) -> coro::CoroTask<void> {
                auto builder = std::make_shared<IndexBuilderUtility>();
                UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                                tags::NeedsContext>
                    exec(builder);
                second = co_await exec.execute(scope, config_force);
            });
        CHECK(second.success);
        CHECK_FALSE(second.was_skipped);
    }

    TEST_CASE("Result has correct line count") {
        const std::size_t n_idx_lines = valgrind_scale(1000, 10);
        TestEnvironment env(n_idx_lines);
        std::string gz_file = env.create_dft_test_gzip_file(n_idx_lines);

        auto config = IndexBuildConfig::for_file(gz_file).with_manifest(false);

        IndexBuildResult result;
        run_coro([&config, &result](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder);
            result = co_await exec.execute(scope, config);
        });

        REQUIRE(result.success);
        CHECK(result.total_lines > 0);
        CHECK(result.total_lines >= static_cast<int>(n_idx_lines));
    }

    TEST_CASE("Incremental manifest add to existing index with bloom") {
        TestEnvironment env(valgrind_scale(1000, 10));
        std::string gz_file =
            env.create_dft_test_gzip_file(valgrind_scale(1000, 10));

        // First build: checkpoint + bloom
        auto config1 = IndexBuildConfig::for_file(gz_file)

                           .with_manifest(false);

        IndexBuildResult r1;
        run_coro([&config1, &r1](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder);
            r1 = co_await exec.execute(scope, config1);
        });
        REQUIRE(r1.success);

        {
            IndexDatabase db(r1.index_path);
            int fid = db.get_file_info_id(internal::get_logical_path(gz_file));
            CHECK(db.has_bloom_data(fid));
            CHECK_FALSE(db.has_manifest_data(fid));
        }

        // Second build: add manifest (bloom already exists, skip it)
        auto config2 = IndexBuildConfig::for_file(gz_file)

                           .with_manifest(true);

        IndexBuildResult r2;
        run_coro([&config2, &r2](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder);
            r2 = co_await exec.execute(scope, config2);
        });
        REQUIRE(r2.success);
        CHECK_FALSE(r2.was_skipped);

        // Verify both bloom and manifest exist
        {
            IndexDatabase db(r2.index_path);
            int fid = db.get_file_info_id(internal::get_logical_path(gz_file));
            CHECK(db.has_bloom_data(fid));
            CHECK(db.has_manifest_data(fid));
        }
    }

    TEST_CASE("Skip when all requested features already exist") {
        TestEnvironment env(valgrind_scale(1000, 10));
        std::string gz_file =
            env.create_dft_test_gzip_file(valgrind_scale(1000, 10));

        // Build with bloom + manifest
        auto config1 = IndexBuildConfig::for_file(gz_file)

                           .with_manifest(true);

        IndexBuildResult r1;
        run_coro([&config1, &r1](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder);
            r1 = co_await exec.execute(scope, config1);
        });
        REQUIRE(r1.success);
        CHECK_FALSE(r1.was_skipped);

        // Build again with same features — should skip
        IndexBuildResult r2;
        run_coro([&config1, &r2](CoroScope& scope) -> coro::CoroTask<void> {
            auto builder = std::make_shared<IndexBuilderUtility>();
            UtilityExecutor<IndexBuildConfig, IndexBuildResult,
                            tags::NeedsContext>
                exec(builder);
            r2 = co_await exec.execute(scope, config1);
        });
        REQUIRE(r2.success);
        CHECK(r2.was_skipped);
    }
}
