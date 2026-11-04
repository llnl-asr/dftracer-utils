// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/runtime/pipeline/scheduler.h>
#include <dftracer/utils/runtime/scheduler/task_executor.h>
#include <dftracer/utils/runtime/scheduler/thread_pool_executor.h>
#include <dftracer/utils/runtime/tasks/task.h>
#include <dftracer/utils/server/trace_index.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <atomic>

using namespace dftracer::utils;
using namespace dftracer::utils::server;

/// Helper: create a DFTracer gzip file with .pfw.gz extension
/// (TraceIndex scans for .pfw and .pfw.gz patterns).
static dstd::String create_pfw_gz(dftu_utils_test::TestEnvironment& env,
                                  int event_count, int id) {
    auto trace_gz = env.create_dft_test_gzip_file(event_count);
    if (trace_gz.empty()) return "";

    // Rename to .pfw.gz so TraceIndex discovers it
    dstd::String pfw_path =
        env.dir() + "/trace_" + dstd::to_string(id) + ".pfw.gz";
    REQUIRE(dstd::fs::rename(trace_gz, pfw_path).has_value());
    return pfw_path;
}

// ============================================================================
// TraceIndex - file discovery
// ============================================================================

TEST_CASE("TraceIndex - discovers .pfw.gz files") {
    dftu_utils_test::TestEnvironment env(100);
    REQUIRE(env.is_valid());

    auto file1 = create_pfw_gz(env, 50, 1);
    auto file2 = create_pfw_gz(env, 100, 2);
    REQUIRE(!file1.empty());
    REQUIRE(!file2.empty());

    ThreadPoolExecutor executor(ExecutorConfig{.thread_count = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> success{false};
    std::size_t file_count = 0;

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            TraceIndex index(dstd::String(env.dir()), dstd::String(env.dir()));
            co_await index.initialize();

            file_count = index.file_count();
            if (file_count >= 2) {
                success.store(true);
            }
            co_return;
        },
        "TraceIndexDiscovery");

    scheduler.schedule(task);
    task->wait();
    CHECK(success.load());
    CHECK(file_count >= 2);

    executor.shutdown();
}

TEST_CASE("TraceIndex - find_file by path") {
    dftu_utils_test::TestEnvironment env(100);
    REQUIRE(env.is_valid());

    auto file = create_pfw_gz(env, 50, 1);
    REQUIRE(!file.empty());

    ThreadPoolExecutor executor(ExecutorConfig{.thread_count = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> found{false};
    dstd::String found_path;

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            TraceIndex index(dstd::String(env.dir()), dstd::String(env.dir()));
            co_await index.initialize();

            auto* info = index.find_file(dstd::String(file));
            if (info != nullptr) {
                found.store(true);
                found_path = info->path;
            }
            co_return;
        },
        "TraceIndexFindFile");

    scheduler.schedule(task);
    task->wait();
    CHECK(found.load());
    CHECK(found_path == file);

    executor.shutdown();
}

TEST_CASE("TraceIndex - find_file returns nullptr for missing file") {
    dftu_utils_test::TestEnvironment env(100);
    REQUIRE(env.is_valid());

    auto file = create_pfw_gz(env, 50, 1);
    REQUIRE(!file.empty());

    ThreadPoolExecutor executor(ExecutorConfig{.thread_count = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> is_null{false};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            TraceIndex index(dstd::String(env.dir()), dstd::String(env.dir()));
            co_await index.initialize();

            auto* info = index.find_file("/nonexistent/path.pfw.gz");
            if (info == nullptr) {
                is_null.store(true);
            }
            co_return;
        },
        "TraceIndexNotFound");

    scheduler.schedule(task);
    task->wait();
    CHECK(is_null.load());

    executor.shutdown();
}

TEST_CASE("TraceIndex - file_at by index") {
    dftu_utils_test::TestEnvironment env(100);
    REQUIRE(env.is_valid());

    auto file = create_pfw_gz(env, 50, 1);
    REQUIRE(!file.empty());

    ThreadPoolExecutor executor(ExecutorConfig{.thread_count = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> success{false};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            TraceIndex index(dstd::String(env.dir()), dstd::String(env.dir()));
            co_await index.initialize();

            REQUIRE(index.file_count() >= 1);
            auto* info = index.file_at(0);
            if (info != nullptr && !info->path.empty()) {
                success.store(true);
            }

            // Out of range returns nullptr
            auto* bad = index.file_at(999);
            REQUIRE(bad == nullptr);
            co_return;
        },
        "TraceIndexFileAt");

    scheduler.schedule(task);
    task->wait();
    CHECK(success.load());

    executor.shutdown();
}

TEST_CASE("TraceIndex - empty directory") {
    auto dir = dftu_utils_test::make_unique_test_path("trace_index_empty");
    REQUIRE(dstd::fs::create_directories(dir).has_value());

    ThreadPoolExecutor executor(ExecutorConfig{.thread_count = 2});
    Scheduler scheduler(&executor);

    std::atomic<std::size_t> count{999};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            TraceIndex index(dstd::String(dir.str()), dstd::String(dir.str()));
            co_await index.initialize();

            count.store(index.file_count());
            co_return;
        },
        "TraceIndexEmpty");

    scheduler.schedule(task);
    task->wait();
    CHECK(count.load() == 0);

    executor.shutdown();
    (void)dstd::fs::remove_all(dir);
}

TEST_CASE("TraceIndex - directory and index_dir accessors") {
    TraceIndex index("/some/dir", "/some/index");

    CHECK(index.directory() == "/some/dir");
    CHECK(index.index_dir() == "/some/index");
    CHECK(index.file_count() == 0);
    CHECK(index.files().empty());
}

TEST_CASE("TraceIndex - genesis output resolves run keys through runs") {
    dftu_utils_test::TestEnvironment env(10);
    REQUIRE(env.is_valid());
    dstd::String text = R"({"gtype":"run","run":"ab","app":"laghos","nodes":4})"
                        "\n";
    for (int i = 0; i < 20; ++i)
        text += R"({"gtype":"func","run":"ab","ts":1,"path":"main;f)" +
                dstd::to_string(i) + R"(","depth":1,"count":2})" + "\n";
    const dstd::String gz = env.dir() + "/genesis.pfw.gz";
    dftu_utils_test::write_gz_trace(gz, text);
    dftracer::utils::index::IndexerOptions o;
    o.index_dir = env.dir();
    dftracer::utils::index::Indexer::open({dstd::String(dstd::StringView(gz))},
                                          o)
        .build();

    ThreadPoolExecutor executor(ExecutorConfig{.thread_count = 2});
    Scheduler scheduler(&executor);
    dstd::String schema, app, nodes;
    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            TraceIndex index(dstd::String(env.dir()), dstd::String(env.dir()));
            co_await index.initialize();
            schema = index.record_schema().id;
            app = index.resolve("runs", "run", "app", "ab");
            nodes = index.resolve("runs", "run", "nodes", "ab");
            co_return;
        },
        "TraceIndexGenesis");
    scheduler.schedule(task);
    task->wait();
    CHECK(schema == "genesis");
    CHECK(app == "laghos");
    CHECK(nodes == "4");
    executor.shutdown();
}
