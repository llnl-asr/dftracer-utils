#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/server/trace_index.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <atomic>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::server;

/// Helper: create a DFTracer gzip file with .pfw.gz extension
/// (TraceIndex scans for .pfw and .pfw.gz patterns).
static std::string create_pfw_gz(dft_utils_test::TestEnvironment& env,
                                 int num_events, int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";

    // Rename to .pfw.gz so TraceIndex discovers it
    std::string pfw_path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    fs::rename(trace_gz, pfw_path);
    return pfw_path;
}

// ============================================================================
// TraceIndex - file discovery
// ============================================================================

TEST_CASE("TraceIndex - discovers .pfw.gz files") {
    dft_utils_test::TestEnvironment env(100);
    REQUIRE(env.is_valid());

    auto file1 = create_pfw_gz(env, 50, 1);
    auto file2 = create_pfw_gz(env, 100, 2);
    REQUIRE(!file1.empty());
    REQUIRE(!file2.empty());

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> success{false};
    std::size_t file_count = 0;

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            TraceIndex index(env.get_dir(), env.get_dir());
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
    dft_utils_test::TestEnvironment env(100);
    REQUIRE(env.is_valid());

    auto file = create_pfw_gz(env, 50, 1);
    REQUIRE(!file.empty());

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> found{false};
    std::string found_path;

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            TraceIndex index(env.get_dir(), env.get_dir());
            co_await index.initialize();

            auto* info = index.find_file(file);
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
    dft_utils_test::TestEnvironment env(100);
    REQUIRE(env.is_valid());

    auto file = create_pfw_gz(env, 50, 1);
    REQUIRE(!file.empty());

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> is_null{false};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            TraceIndex index(env.get_dir(), env.get_dir());
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
    dft_utils_test::TestEnvironment env(100);
    REQUIRE(env.is_valid());

    auto file = create_pfw_gz(env, 50, 1);
    REQUIRE(!file.empty());

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> success{false};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            TraceIndex index(env.get_dir(), env.get_dir());
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
    auto dir = dft_utils_test::make_unique_test_path("trace_index_empty");
    fs::create_directories(dir);

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<std::size_t> count{999};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            TraceIndex index(dir.string(), dir.string());
            co_await index.initialize();

            count.store(index.file_count());
            co_return;
        },
        "TraceIndexEmpty");

    scheduler.schedule(task);
    task->wait();
    CHECK(count.load() == 0);

    executor.shutdown();
    fs::remove_all(dir);
}

TEST_CASE("TraceIndex - directory and index_dir accessors") {
    TraceIndex index("/some/dir", "/some/index");

    CHECK(index.directory() == "/some/dir");
    CHECK(index.index_dir() == "/some/index");
    CHECK(index.file_count() == 0);
    CHECK(index.files().empty());
}
