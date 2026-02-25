#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/core/io/io_backend.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dftracer::utils;
namespace aio = dftracer::utils::io;

// Helper: create a temp file with known content, return path.
static std::string create_temp_file(const std::string& content) {
    char path[] = "/tmp/dftracer_io_test_XXXXXX";
    int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);
    ssize_t written = ::write(fd, content.data(), content.size());
    REQUIRE(written == static_cast<ssize_t>(content.size()));
    ::close(fd);
    return std::string(path);
}

// ============================================================================
// Sync fallback tests (no executor)
// ============================================================================

TEST_CASE("AsyncIO - sync fallback: async_read without executor") {
    std::string data = "Hello, async I/O world!";
    auto path = create_temp_file(data);

    int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);

    // No executor -- Executor::current() is nullptr.
    // async_read should fall back to synchronous pread().
    char buf[64] = {};
    auto awaitable = aio::read(fd, buf, data.size(), 0);
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == static_cast<ssize_t>(data.size()));
    CHECK(std::string(buf, static_cast<std::size_t>(awaitable.result_)) ==
          data);

    ::close(fd);
    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - sync fallback: async_write without executor") {
    char path[] = "/tmp/dftracer_io_write_XXXXXX";
    int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);

    std::string data = "write test data";
    auto awaitable = aio::write(fd, data.data(), data.size(), 0);
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == static_cast<ssize_t>(data.size()));

    // Read back and verify
    char buf[64] = {};
    ::lseek(fd, 0, SEEK_SET);
    ssize_t n = ::read(fd, buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(data.size()));
    CHECK(std::string(buf, static_cast<std::size_t>(n)) == data);

    ::close(fd);
    ::unlink(path);
}

TEST_CASE("AsyncIO - sync fallback: async_open/close without executor") {
    std::string data = "open test";
    auto path = create_temp_file(data);

    auto open_result = aio::open(path.c_str(), O_RDONLY);
    CHECK(open_result.ready_);
    CHECK(open_result.result_ >= 0);

    int fd = static_cast<int>(open_result.result_);
    auto close_result = aio::close(fd);
    CHECK(close_result.ready_);
    CHECK(close_result.result_ == 0);

    ::unlink(path.c_str());
}

// ============================================================================
// Async tests (with executor + thread pool backend)
// ============================================================================

TEST_CASE("AsyncIO - async_read with executor") {
    std::string data = "executor async read test data!";
    auto path = create_temp_file(data);

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> success{false};
    std::string read_data;

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            int fd = ::open(path.c_str(), O_RDONLY);
            REQUIRE(fd >= 0);

            char buf[128] = {};
            ssize_t n = co_await aio::read(fd, buf, data.size(), 0);
            if (n == static_cast<ssize_t>(data.size())) {
                read_data.assign(buf, static_cast<std::size_t>(n));
                success.store(true);
            }

            ::close(fd);
            co_return;
        },
        "AsyncReadTest");

    scheduler.schedule(task);
    task->wait();
    CHECK(success.load());
    CHECK(read_data == data);

    executor.shutdown();
    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - async_write with executor") {
    char path[] = "/tmp/dftracer_io_async_write_XXXXXX";
    int tmpfd = ::mkstemp(path);
    REQUIRE(tmpfd >= 0);
    ::close(tmpfd);

    std::string data = "executor async write test!";

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> success{false};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            int fd = ::open(path, O_WRONLY | O_TRUNC);
            REQUIRE(fd >= 0);

            ssize_t n = co_await aio::write(fd, data.data(), data.size(), 0);
            if (n == static_cast<ssize_t>(data.size())) {
                success.store(true);
            }

            ::close(fd);
            co_return;
        },
        "AsyncWriteTest");

    scheduler.schedule(task);
    task->wait();
    CHECK(success.load());

    // Read back and verify
    char buf[128] = {};
    int rfd = ::open(path, O_RDONLY);
    REQUIRE(rfd >= 0);
    ssize_t n = ::read(rfd, buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(data.size()));
    CHECK(std::string(buf, static_cast<std::size_t>(n)) == data);
    ::close(rfd);

    executor.shutdown();
    ::unlink(path);
}

TEST_CASE("AsyncIO - async_open + async_read + async_close lifecycle") {
    std::string data = "full lifecycle test";
    auto path = create_temp_file(data);

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> success{false};
    std::string read_data;

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            ssize_t fd_result = co_await aio::open(path.c_str(), O_RDONLY);
            REQUIRE(fd_result >= 0);
            int fd = static_cast<int>(fd_result);

            char buf[128] = {};
            ssize_t n = co_await aio::read(fd, buf, data.size(), 0);
            if (n == static_cast<ssize_t>(data.size())) {
                read_data.assign(buf, static_cast<std::size_t>(n));
            }

            ssize_t close_result = co_await aio::close(fd);
            if (close_result == 0 && n == static_cast<ssize_t>(data.size())) {
                success.store(true);
            }

            co_return;
        },
        "LifecycleTest");

    scheduler.schedule(task);
    task->wait();
    CHECK(success.load());
    CHECK(read_data == data);

    executor.shutdown();
    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - multiple concurrent async reads") {
    constexpr int N = 8;
    std::vector<std::string> paths;
    std::vector<std::string> expected;

    for (int i = 0; i < N; ++i) {
        std::string content =
            "file " + std::to_string(i) + " content that is unique";
        paths.push_back(create_temp_file(content));
        expected.push_back(content);
    }

    Executor executor(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(&executor);

    std::atomic<int> completed{0};
    std::vector<std::string> results(N);

    auto task = make_task(
        [&](CoroScope& scope) -> coro::CoroTask<void> {
            for (int i = 0; i < N; ++i) {
                auto* path_ptr = &paths[i];
                auto* expected_ptr = &expected[i];
                auto* result_ptr = &results[i];
                auto* completed_ptr = &completed;

                scope.spawn([path_ptr, expected_ptr, result_ptr, completed_ptr](
                                CoroScope& /*s*/) -> coro::CoroTask<void> {
                    int fd = ::open(path_ptr->c_str(), O_RDONLY);
                    if (fd < 0) co_return;

                    char buf[256] = {};
                    ssize_t n =
                        co_await aio::read(fd, buf, expected_ptr->size(), 0);
                    if (n > 0) {
                        result_ptr->assign(buf, static_cast<std::size_t>(n));
                    }

                    ::close(fd);
                    completed_ptr->fetch_add(1);
                    co_return;
                });
            }

            co_await scope.join();
            co_return;
        },
        "ConcurrentReads");

    scheduler.schedule(task);
    task->wait();
    CHECK(completed.load() == N);
    for (int i = 0; i < N; ++i) {
        CHECK(results[i] == expected[i]);
    }

    executor.shutdown();
    for (auto& p : paths) ::unlink(p.c_str());
}

TEST_CASE("AsyncIO - async_read error handling (invalid fd)") {
    auto awaitable = aio::read(-1, nullptr, 0, 0);
    // With no executor, sync fallback calls pread(-1, ...) which fails
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ < 0);
}

TEST_CASE("AsyncIO - IoBackend::name() reports backend type") {
    Executor executor(ExecutorConfig{.num_threads = 1});
    Scheduler scheduler(&executor);

    std::string backend_name;

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            auto* exec = Executor::current();
            REQUIRE(exec != nullptr);
            REQUIRE(exec->has_io_backend());
            backend_name = exec->io_backend().name();
            co_return;
        },
        "BackendNameTest");

    scheduler.schedule(task);
    task->wait();
    CHECK(!backend_name.empty());
    MESSAGE("Active I/O backend: ", backend_name);
    CHECK((backend_name == "threadpool" || backend_name == "io_uring" ||
           backend_name == "epoll+threadpool" ||
           backend_name == "kqueue+threadpool"));

    executor.shutdown();
}

TEST_CASE("AsyncIO - async_read error handling with executor (invalid fd)") {
    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<ssize_t> result{0};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            char buf[16] = {};
            ssize_t n = co_await aio::read(-1, buf, 16, 0);
            result.store(n);
            co_return;
        },
        "ErrorHandlingTest");

    scheduler.schedule(task);
    task->wait();
    CHECK(result.load() < 0);

    executor.shutdown();
}

TEST_CASE("AsyncIO - large read with executor") {
    // Write 1MB of data
    constexpr std::size_t SIZE = 1024 * 1024;
    std::string data(SIZE, 'A');
    for (std::size_t i = 0; i < SIZE; ++i) {
        data[i] = static_cast<char>('A' + (i % 26));
    }
    auto path = create_temp_file(data);

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> success{false};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            int fd = ::open(path.c_str(), O_RDONLY);
            REQUIRE(fd >= 0);

            std::vector<char> buf(SIZE);
            ssize_t n = co_await aio::read(fd, buf.data(), SIZE, 0);
            if (n == static_cast<ssize_t>(SIZE) &&
                std::memcmp(buf.data(), data.data(), SIZE) == 0) {
                success.store(true);
            }

            ::close(fd);
            co_return;
        },
        "LargeReadTest");

    scheduler.schedule(task);
    task->wait();
    CHECK(success.load());

    executor.shutdown();
    ::unlink(path.c_str());
}

// ============================================================================
// Per-backend tests: force each backend and run the full lifecycle
// ============================================================================

/// Helper: run the open+read+close lifecycle on a forced backend.
static void run_lifecycle_on_backend(io::IoBackendType type,
                                     const char* type_name) {
    std::string data = std::string("backend test: ") + type_name;
    auto path = create_temp_file(data);

    Executor executor(
        ExecutorConfig{.num_threads = 2, .io_backend_type = type});
    Scheduler scheduler(&executor);

    std::atomic<bool> success{false};
    std::string read_data;
    std::string backend_name;

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            auto* exec = Executor::current();
            REQUIRE(exec != nullptr);
            REQUIRE(exec->has_io_backend());
            backend_name = exec->io_backend().name();

            ssize_t fd_result = co_await aio::open(path.c_str(), O_RDONLY);
            REQUIRE(fd_result >= 0);
            int fd = static_cast<int>(fd_result);

            char buf[128] = {};
            ssize_t n = co_await aio::read(fd, buf, data.size(), 0);
            if (n == static_cast<ssize_t>(data.size())) {
                read_data.assign(buf, static_cast<std::size_t>(n));
            }

            ssize_t close_result = co_await aio::close(fd);
            if (close_result == 0 && n == static_cast<ssize_t>(data.size())) {
                success.store(true);
            }

            co_return;
        },
        std::string("BackendLifecycle[") + type_name + "]");

    scheduler.schedule(task);
    task->wait();
    MESSAGE("Backend requested: ", type_name, ", actual: ", backend_name);
    CHECK(success.load());
    CHECK(read_data == data);

    executor.shutdown();
    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - lifecycle on threadpool backend (forced)") {
    run_lifecycle_on_backend(io::IoBackendType::THREADPOOL, "threadpool");
}

#ifdef __linux__
TEST_CASE("AsyncIO - lifecycle on epoll+threadpool backend (forced)") {
    run_lifecycle_on_backend(io::IoBackendType::EPOLL_THREADPOOL,
                             "epoll+threadpool");
}
#endif

#ifdef DFTRACER_UTILS_HAVE_IO_URING
TEST_CASE("AsyncIO - lifecycle on io_uring backend (forced)") {
    run_lifecycle_on_backend(io::IoBackendType::IO_URING, "io_uring");
}
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
TEST_CASE("AsyncIO - lifecycle on kqueue+threadpool backend (forced)") {
    run_lifecycle_on_backend(io::IoBackendType::KQUEUE_THREADPOOL,
                             "kqueue+threadpool");
}
#endif
