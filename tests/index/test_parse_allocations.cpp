#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <sstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;
namespace store = dftracer::utils::index::store;

namespace {

constexpr int FILES = 8;
constexpr int EVENTS = 4000;
// 14.1 per event before; 6.6 once the parse kept no args map it never read
// and every file's fold driver shared one pool of event buffers.
constexpr double MAX_PER_EVENT = 8.0;
// 4,250 per file of 500 events before; 3,527 once a fresh file id skipped
// its tier clears.
constexpr double MAX_PER_FILE = 3900.0;

std::string write_trace(const std::string& dir, int n, int events = EVENTS) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < events; ++i)
        out << R"({"id":)" << i << R"(,"name":"read","cat":"POSIX","pid":)"
            << 1 + i % 4 << R"(,"tid":1,"ph":"X","ts":)" << 1000 + i * 10
            << R"(,"dur":3,"args":{"fname":"/data/file_number_)" << n << "_"
            << i % 37 << R"(","size":)" << i % 4096
            << R"(,"hhash":"a1b2c3d4e5f60718","level":)" << i % 3 << "}}\n";
    out << "]\n";
    return dftu_utils_test::write_gz_trace(
        dir + "/t" + std::to_string(n) + ".pfw.gz", out.str());
}

}  // namespace

TEST_SUITE("IndexParseAllocations") {
    TEST_CASE("the index parse makes few global allocations per event") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < FILES; ++i)
            files.push_back(write_trace(env.get_dir(), i));

        Runtime rt(4);
        build::IndexBuildBatchResult result;
        const auto pool_before = memory::debug_allocation_count();
        auto task = run_coro_scope(
            rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
                auto config = std::make_shared<build::IndexBuildBatchConfig>();
                config->file_paths = files;
                config->index_dir = env.get_dir() + "/idx";
                config->parallelism = 4;
                result = co_await build::BatchBuilder::process(
                    &scope, std::move(config));
            });
        rt.submit(std::move(task), "parse-allocations").wait();
        const auto pool = memory::debug_allocation_count() - pool_before;
        REQUIRE(pool > 0);
        rt.shutdown();

        REQUIRE(result.indexed == FILES);
        const double per_event = static_cast<double>(pool) / (FILES * EVENTS);
        MESSAGE("global allocations per event: " << per_event);
        CHECK(per_event < MAX_PER_EVENT);
    }

    TEST_CASE(
        "a fresh build of many small files makes few allocations "
        "per file") {
        // Small files make the per-file work count: each new file's tier
        // clears were range deletes, each a memtable allocation, though a
        // fresh file id has nothing to clear.
        constexpr int SMALL_FILES = 64;
        constexpr int SMALL_EVENTS = 500;
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < SMALL_FILES; ++i)
            files.push_back(write_trace(env.get_dir(), i, SMALL_EVENTS));

        Runtime rt(4);
        build::IndexBuildBatchResult result;
        const auto pool_before = memory::debug_allocation_count();
        auto task = run_coro_scope(
            rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
                auto config = std::make_shared<build::IndexBuildBatchConfig>();
                config->file_paths = files;
                config->index_dir = env.get_dir() + "/idx";
                config->parallelism = 4;
                result = co_await build::BatchBuilder::process(
                    &scope, std::move(config));
            });
        rt.submit(std::move(task), "parse-allocations-small").wait();
        const auto pool = memory::debug_allocation_count() - pool_before;
        REQUIRE(pool > 0);
        rt.shutdown();

        REQUIRE(result.indexed == SMALL_FILES);
        const double per_file = static_cast<double>(pool) / SMALL_FILES;
        MESSAGE("global allocations per file: " << per_file);
        CHECK(per_file < MAX_PER_FILE);
    }

    TEST_CASE(
        "clears remove the data of a rebuilt file id and spare a fresh one") {
        constexpr int REBUILT = 1;
        constexpr int FRESH = 2;
        const auto ext = store::IndexExtension::Bloom;
        dftu_utils_test::TestEnvironment env(10);
        store::IndexDatabase db(env.get_dir() + "/clears_idx");
        {
            auto w = db.begin_write();
            for (int id : {REBUILT, FRESH}) {
                store::records::put_manifest(*w, id, ext, 0);
                store::records::put_path(*w, ext, id, "p");
                store::records::put_path_file(*w, ext, id, "p", "payload");
            }
            w->commit();
        }
        REQUIRE(db.extension_current(REBUILT, ext));
        REQUIRE(db.path_file_value(REBUILT, ext, "p").has_value());

        {
            auto w = db.begin_write();
            w->set_fresh_file(FRESH);
            store::records::clear_path(*w, ext, REBUILT, "p");
            store::records::clear_path(*w, ext, FRESH, "p");
            w->commit();
        }
        CHECK_FALSE(db.path_file_value(REBUILT, ext, "p").has_value());
        CHECK(db.path_file_value(FRESH, ext, "p").has_value());

        {
            auto w = db.begin_write();
            w->set_fresh_file(FRESH);
            store::records::clear_file(*w, ext, REBUILT);
            store::records::clear_file(*w, ext, FRESH);
            w->commit();
        }
        CHECK_FALSE(db.extension_current(REBUILT, ext));
        CHECK(db.extension_paths(REBUILT, ext).empty());
        CHECK(db.extension_current(FRESH, ext));
        CHECK(db.extension_paths(FRESH, ext).size() == 1);
    }
}
