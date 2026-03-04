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
#include <sys/uio.h>
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
// Sync fallback tests (no executor) — sequential io::read / io::write
// ============================================================================

TEST_CASE("AsyncIO - sync fallback: read without executor") {
    std::string data = "Hello, async I/O world!";
    auto path = create_temp_file(data);

    int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);

    // No executor -- falls back to synchronous ::read()
    char buf[64] = {};
    auto awaitable = aio::read(fd, buf, data.size());
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == static_cast<ssize_t>(data.size()));
    CHECK(std::string(buf, static_cast<std::size_t>(awaitable.result_)) ==
          data);

    ::close(fd);
    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - sync fallback: write without executor") {
    char path[] = "/tmp/dftracer_io_write_XXXXXX";
    int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);

    std::string data = "write test data";
    auto awaitable = aio::write(fd, data.data(), data.size());
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

TEST_CASE("AsyncIO - sync fallback: open/close without executor") {
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
// Sync fallback tests — positional io::pread / io::pwrite
// ============================================================================

TEST_CASE("AsyncIO - sync fallback: pread without executor") {
    std::string data = "Hello, positional I/O!";
    auto path = create_temp_file(data);

    int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);

    // Read from offset 7
    char buf[64] = {};
    auto awaitable = aio::pread(fd, buf, 15, 7);
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == 15);
    CHECK(std::string(buf, 15) == "positional I/O!");

    ::close(fd);
    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - sync fallback: pwrite without executor") {
    char path[] = "/tmp/dftracer_io_pwrite_XXXXXX";
    int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);

    // Write initial content
    std::string initial = "AAAAAAAAAA";
    (void)::write(fd, initial.data(), initial.size());

    // Positional write at offset 3
    std::string patch = "BBB";
    auto awaitable = aio::pwrite(fd, patch.data(), patch.size(), 3);
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == static_cast<ssize_t>(patch.size()));

    // Read back and verify
    char buf[64] = {};
    ::lseek(fd, 0, SEEK_SET);
    ssize_t n = ::read(fd, buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(initial.size()));
    CHECK(std::string(buf, static_cast<std::size_t>(n)) == "AAABBBAAAA");

    ::close(fd);
    ::unlink(path);
}

// ============================================================================
// Key regression test: write to non-seekable fd (pipe)
// ============================================================================

TEST_CASE("AsyncIO - sync fallback: write to pipe (non-seekable fd)") {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    std::string data = "pipe write test";

    // io::write should use ::write() which works on pipes.
    // Previously used ::pwrite() which fails with ESPIPE on pipes.
    auto awaitable = aio::write(pipefd[1], data.data(), data.size());
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == static_cast<ssize_t>(data.size()));

    // Read back from pipe
    char buf[64] = {};
    ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(data.size()));
    CHECK(std::string(buf, static_cast<std::size_t>(n)) == data);

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

TEST_CASE("AsyncIO - sync fallback: read from pipe (non-seekable fd)") {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    std::string data = "pipe read test";
    ssize_t written = ::write(pipefd[1], data.data(), data.size());
    REQUIRE(written == static_cast<ssize_t>(data.size()));
    ::close(pipefd[1]);  // close write end so read sees EOF after data

    // io::read should use ::read() which works on pipes.
    char buf[64] = {};
    auto awaitable = aio::read(pipefd[0], buf, data.size());
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == static_cast<ssize_t>(data.size()));
    CHECK(std::string(buf, static_cast<std::size_t>(awaitable.result_)) ==
          data);

    ::close(pipefd[0]);
}

TEST_CASE("AsyncIO - pwrite fails on pipe (expected ESPIPE)") {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    // pwrite on a pipe should fail with -ESPIPE
    std::string data = "should fail";
    auto awaitable = aio::pwrite(pipefd[1], data.data(), data.size(), 0);
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == -ESPIPE);

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

TEST_CASE("AsyncIO - pread fails on pipe (expected ESPIPE)") {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    char buf[16] = {};
    auto awaitable = aio::pread(pipefd[0], buf, sizeof(buf), 0);
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == -ESPIPE);

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

// ============================================================================
// Sequential read/write maintain file position
// ============================================================================

TEST_CASE("AsyncIO - sequential writes advance file position") {
    char path[] = "/tmp/dftracer_io_seq_write_XXXXXX";
    int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);

    // Two sequential writes should append
    std::string part1 = "Hello, ";
    std::string part2 = "World!";
    auto a1 = aio::write(fd, part1.data(), part1.size());
    CHECK(a1.ready_);
    CHECK(a1.result_ == static_cast<ssize_t>(part1.size()));

    auto a2 = aio::write(fd, part2.data(), part2.size());
    CHECK(a2.ready_);
    CHECK(a2.result_ == static_cast<ssize_t>(part2.size()));

    // Read back
    char buf[64] = {};
    ::lseek(fd, 0, SEEK_SET);
    ssize_t n = ::read(fd, buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(part1.size() + part2.size()));
    CHECK(std::string(buf, static_cast<std::size_t>(n)) == "Hello, World!");

    ::close(fd);
    ::unlink(path);
}

TEST_CASE("AsyncIO - sequential reads advance file position") {
    std::string data = "ABCDEFGHIJ";
    auto path = create_temp_file(data);

    int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);

    // Two sequential reads should advance position
    char buf1[5] = {};
    auto a1 = aio::read(fd, buf1, 5);
    CHECK(a1.ready_);
    CHECK(a1.result_ == 5);
    CHECK(std::string(buf1, 5) == "ABCDE");

    char buf2[5] = {};
    auto a2 = aio::read(fd, buf2, 5);
    CHECK(a2.ready_);
    CHECK(a2.result_ == 5);
    CHECK(std::string(buf2, 5) == "FGHIJ");

    ::close(fd);
    ::unlink(path.c_str());
}

// ============================================================================
// Positional read/write do NOT change file position
// ============================================================================

TEST_CASE("AsyncIO - pread does not change file position") {
    std::string data = "ABCDEFGHIJ";
    auto path = create_temp_file(data);

    int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);

    // pread at offset 5
    char buf1[5] = {};
    auto a1 = aio::pread(fd, buf1, 5, 5);
    CHECK(a1.result_ == 5);
    CHECK(std::string(buf1, 5) == "FGHIJ");

    // File position should still be 0 — sequential read should get "ABCDE"
    char buf2[5] = {};
    auto a2 = aio::read(fd, buf2, 5);
    CHECK(a2.result_ == 5);
    CHECK(std::string(buf2, 5) == "ABCDE");

    ::close(fd);
    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - pwrite does not change file position") {
    char path[] = "/tmp/dftracer_io_pwrite_pos_XXXXXX";
    int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);

    std::string initial = "AAAAAAAAAA";
    (void)::write(fd, initial.data(), initial.size());

    // Seek to position 0
    ::lseek(fd, 0, SEEK_SET);

    // pwrite at offset 5 — should NOT change current position
    std::string patch = "BBB";
    auto a1 = aio::pwrite(fd, patch.data(), patch.size(), 5);
    CHECK(a1.result_ == static_cast<ssize_t>(patch.size()));

    // Sequential write at current position (0) should overwrite start
    std::string seq = "CC";
    auto a2 = aio::write(fd, seq.data(), seq.size());
    CHECK(a2.result_ == static_cast<ssize_t>(seq.size()));

    // Read back — expect "CCAAABBBAA"
    char buf[64] = {};
    ::lseek(fd, 0, SEEK_SET);
    ssize_t n = ::read(fd, buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(initial.size()));
    CHECK(std::string(buf, static_cast<std::size_t>(n)) == "CCAAABBBAA");

    ::close(fd);
    ::unlink(path);
}

// ============================================================================
// Async tests (with executor + thread pool backend)
// ============================================================================

TEST_CASE("AsyncIO - async read with executor") {
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
            ssize_t n = co_await aio::pread(fd, buf, data.size(), 0);
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

TEST_CASE("AsyncIO - async write with executor") {
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

            ssize_t n = co_await aio::write(fd, data.data(), data.size());
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

TEST_CASE("AsyncIO - async pread with executor") {
    std::string data = "ABCDEFGHIJKLMNOP";
    auto path = create_temp_file(data);

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> success{false};
    std::string read_data;

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            int fd = ::open(path.c_str(), O_RDONLY);
            REQUIRE(fd >= 0);

            // Read 5 bytes from offset 4
            char buf[16] = {};
            ssize_t n = co_await aio::pread(fd, buf, 5, 4);
            if (n == 5) {
                read_data.assign(buf, 5);
                success.store(true);
            }

            ::close(fd);
            co_return;
        },
        "AsyncPreadTest");

    scheduler.schedule(task);
    task->wait();
    CHECK(success.load());
    CHECK(read_data == "EFGHI");

    executor.shutdown();
    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - async pwrite with executor") {
    char path[] = "/tmp/dftracer_io_async_pwrite_XXXXXX";
    int tmpfd = ::mkstemp(path);
    REQUIRE(tmpfd >= 0);
    std::string initial = "AAAAAAAAAA";
    (void)::write(tmpfd, initial.data(), initial.size());
    ::close(tmpfd);

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> success{false};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            int fd = ::open(path, O_WRONLY);
            REQUIRE(fd >= 0);

            std::string patch = "BBB";
            ssize_t n = co_await aio::pwrite(fd, patch.data(), patch.size(), 3);
            if (n == static_cast<ssize_t>(patch.size())) {
                success.store(true);
            }

            ::close(fd);
            co_return;
        },
        "AsyncPwriteTest");

    scheduler.schedule(task);
    task->wait();
    CHECK(success.load());

    // Read back and verify
    char buf[64] = {};
    int rfd = ::open(path, O_RDONLY);
    REQUIRE(rfd >= 0);
    ssize_t n = ::read(rfd, buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(initial.size()));
    CHECK(std::string(buf, static_cast<std::size_t>(n)) == "AAABBBAAAA");
    ::close(rfd);

    executor.shutdown();
    ::unlink(path);
}

TEST_CASE("AsyncIO - async write to pipe with executor") {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::string data = "async pipe write";
    std::atomic<bool> success{false};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            ssize_t n =
                co_await aio::write(pipefd[1], data.data(), data.size());
            if (n == static_cast<ssize_t>(data.size())) {
                success.store(true);
            }
            co_return;
        },
        "AsyncPipeWriteTest");

    scheduler.schedule(task);
    task->wait();
    CHECK(success.load());

    // Read back from pipe
    char buf[64] = {};
    ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(data.size()));
    CHECK(std::string(buf, static_cast<std::size_t>(n)) == data);

    ::close(pipefd[0]);
    ::close(pipefd[1]);
    executor.shutdown();
}

TEST_CASE("AsyncIO - async open + pread + close lifecycle") {
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
            ssize_t n = co_await aio::pread(fd, buf, data.size(), 0);
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
                        co_await aio::pread(fd, buf, expected_ptr->size(), 0);
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

TEST_CASE("AsyncIO - read error handling (invalid fd)") {
    auto awaitable = aio::read(-1, nullptr, 0);
    // With no executor, sync fallback calls read(-1, ...) which fails
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

TEST_CASE("AsyncIO - async read error handling with executor (invalid fd)") {
    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<ssize_t> result{0};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            char buf[16] = {};
            ssize_t n = co_await aio::read(-1, buf, 16);
            result.store(n);
            co_return;
        },
        "ErrorHandlingTest");

    scheduler.schedule(task);
    task->wait();
    CHECK(result.load() < 0);

    executor.shutdown();
}

TEST_CASE("AsyncIO - large pread with executor") {
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
            ssize_t n = co_await aio::pread(fd, buf.data(), SIZE, 0);
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

/// Helper: run the open+pread+close lifecycle on a forced backend.
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
            ssize_t n = co_await aio::pread(fd, buf, data.size(), 0);
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

/// Helper: run write to pipe on a forced backend (regression test).
static void run_pipe_write_on_backend(io::IoBackendType type,
                                      const char* type_name) {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    Executor executor(
        ExecutorConfig{.num_threads = 2, .io_backend_type = type});
    Scheduler scheduler(&executor);

    std::string data = std::string("pipe write on ") + type_name;
    std::atomic<bool> success{false};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            ssize_t n =
                co_await aio::write(pipefd[1], data.data(), data.size());
            if (n == static_cast<ssize_t>(data.size())) {
                success.store(true);
            }
            co_return;
        },
        std::string("PipeWrite[") + type_name + "]");

    scheduler.schedule(task);
    task->wait();
    CHECK(success.load());

    // Read back from pipe
    char buf[128] = {};
    ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(data.size()));
    CHECK(std::string(buf, static_cast<std::size_t>(n)) == data);

    ::close(pipefd[0]);
    ::close(pipefd[1]);
    executor.shutdown();
}

TEST_CASE("AsyncIO - lifecycle on threadpool backend (forced)") {
    run_lifecycle_on_backend(io::IoBackendType::THREADPOOL, "threadpool");
}

TEST_CASE("AsyncIO - pipe write on threadpool backend (forced)") {
    run_pipe_write_on_backend(io::IoBackendType::THREADPOOL, "threadpool");
}

#ifdef __linux__
TEST_CASE("AsyncIO - lifecycle on epoll+threadpool backend (forced)") {
    run_lifecycle_on_backend(io::IoBackendType::EPOLL_THREADPOOL,
                             "epoll+threadpool");
}

TEST_CASE("AsyncIO - pipe write on epoll+threadpool backend (forced)") {
    run_pipe_write_on_backend(io::IoBackendType::EPOLL_THREADPOOL,
                              "epoll+threadpool");
}
#endif

#ifdef DFTRACER_UTILS_HAVE_IO_URING
TEST_CASE("AsyncIO - lifecycle on io_uring backend (forced)") {
    run_lifecycle_on_backend(io::IoBackendType::IO_URING, "io_uring");
}

TEST_CASE("AsyncIO - pipe write on io_uring backend (forced)") {
    run_pipe_write_on_backend(io::IoBackendType::IO_URING, "io_uring");
}
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
TEST_CASE("AsyncIO - lifecycle on kqueue+threadpool backend (forced)") {
    run_lifecycle_on_backend(io::IoBackendType::KQUEUE_THREADPOOL,
                             "kqueue+threadpool");
}

TEST_CASE("AsyncIO - pipe write on kqueue+threadpool backend (forced)") {
    run_pipe_write_on_backend(io::IoBackendType::KQUEUE_THREADPOOL,
                              "kqueue+threadpool");
}
#endif

// ============================================================================
// Scatter-gather I/O: readv / writev
// ============================================================================

TEST_CASE("AsyncIO - sync fallback: writev without executor") {
    char path[] = "/tmp/dftracer_io_writev_XXXXXX";
    int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);

    std::string p1 = "Hello, ";
    std::string p2 = "World!";
    struct iovec iov[2];
    iov[0].iov_base = const_cast<char*>(p1.data());
    iov[0].iov_len = p1.size();
    iov[1].iov_base = const_cast<char*>(p2.data());
    iov[1].iov_len = p2.size();

    auto awaitable = aio::writev(fd, iov, 2);
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == static_cast<ssize_t>(p1.size() + p2.size()));

    // Read back
    char buf[64] = {};
    ::lseek(fd, 0, SEEK_SET);
    ssize_t n = ::read(fd, buf, sizeof(buf));
    CHECK(std::string(buf, static_cast<std::size_t>(n)) == "Hello, World!");

    ::close(fd);
    ::unlink(path);
}

TEST_CASE("AsyncIO - sync fallback: readv without executor") {
    std::string data = "ABCDEFGHIJ";
    auto path = create_temp_file(data);

    int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);

    char buf1[5] = {};
    char buf2[5] = {};
    struct iovec iov[2];
    iov[0].iov_base = buf1;
    iov[0].iov_len = 5;
    iov[1].iov_base = buf2;
    iov[1].iov_len = 5;

    auto awaitable = aio::readv(fd, iov, 2);
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == 10);
    CHECK(std::string(buf1, 5) == "ABCDE");
    CHECK(std::string(buf2, 5) == "FGHIJ");

    ::close(fd);
    ::unlink(path.c_str());
}

// ============================================================================
// Scatter-gather positional I/O: preadv / pwritev
// ============================================================================

TEST_CASE("AsyncIO - sync fallback: pwritev without executor") {
    char path[] = "/tmp/dftracer_io_pwritev_XXXXXX";
    int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);

    std::string initial = "AAAAAAAAAA";
    (void)::write(fd, initial.data(), initial.size());

    std::string p1 = "BB";
    std::string p2 = "CC";
    struct iovec iov[2];
    iov[0].iov_base = const_cast<char*>(p1.data());
    iov[0].iov_len = p1.size();
    iov[1].iov_base = const_cast<char*>(p2.data());
    iov[1].iov_len = p2.size();

    auto awaitable = aio::pwritev(fd, iov, 2, 3);
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == 4);

    char buf[64] = {};
    ::lseek(fd, 0, SEEK_SET);
    ssize_t n = ::read(fd, buf, sizeof(buf));
    CHECK(std::string(buf, static_cast<std::size_t>(n)) == "AAABBCCAAA");

    ::close(fd);
    ::unlink(path);
}

TEST_CASE("AsyncIO - sync fallback: preadv without executor") {
    std::string data = "ABCDEFGHIJ";
    auto path = create_temp_file(data);

    int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);

    char buf1[3] = {};
    char buf2[4] = {};
    struct iovec iov[2];
    iov[0].iov_base = buf1;
    iov[0].iov_len = 3;
    iov[1].iov_base = buf2;
    iov[1].iov_len = 4;

    auto awaitable = aio::preadv(fd, iov, 2, 2);
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == 7);
    CHECK(std::string(buf1, 3) == "CDE");
    CHECK(std::string(buf2, 4) == "FGHI");

    // File position should remain at 0
    char pos_buf[5] = {};
    ssize_t n = ::read(fd, pos_buf, 5);
    CHECK(std::string(pos_buf, static_cast<std::size_t>(n)) == "ABCDE");

    ::close(fd);
    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - writev to pipe (non-seekable fd)") {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    std::string p1 = "pipe ";
    std::string p2 = "scatter";
    struct iovec iov[2];
    iov[0].iov_base = const_cast<char*>(p1.data());
    iov[0].iov_len = p1.size();
    iov[1].iov_base = const_cast<char*>(p2.data());
    iov[1].iov_len = p2.size();

    auto awaitable = aio::writev(pipefd[1], iov, 2);
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == static_cast<ssize_t>(p1.size() + p2.size()));

    char buf[64] = {};
    ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
    CHECK(std::string(buf, static_cast<std::size_t>(n)) == "pipe scatter");

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

// ============================================================================
// lseek
// ============================================================================

TEST_CASE("AsyncIO - sync fallback: lseek without executor") {
    std::string data = "ABCDEFGHIJ";
    auto path = create_temp_file(data);

    int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);

    // Seek to offset 5
    auto awaitable = aio::lseek(fd, 5, SEEK_SET);
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == 5);

    // Sequential read should start from offset 5
    char buf[5] = {};
    ssize_t n = ::read(fd, buf, 5);
    CHECK(std::string(buf, static_cast<std::size_t>(n)) == "FGHIJ");

    // Seek relative
    auto a2 = aio::lseek(fd, -3, SEEK_CUR);
    CHECK(a2.ready_);
    CHECK(a2.result_ == 7);

    // Seek to end
    auto a3 = aio::lseek(fd, 0, SEEK_END);
    CHECK(a3.ready_);
    CHECK(a3.result_ == static_cast<ssize_t>(data.size()));

    ::close(fd);
    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - lseek on pipe fails (expected ESPIPE)") {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    auto awaitable = aio::lseek(pipefd[0], 0, SEEK_SET);
    CHECK(awaitable.ready_);
    CHECK(awaitable.result_ == -ESPIPE);

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

// ============================================================================
// sendfile
// ============================================================================

TEST_CASE("AsyncIO - sync fallback: sendfile without executor") {
    std::string data = "sendfile test data for zero-copy";
    auto path = create_temp_file(data);

    int in_fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(in_fd >= 0);

    // Create output pipe to receive the data
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    auto awaitable = aio::sendfile(pipefd[1], in_fd, 0, data.size());
    CHECK(awaitable.ready_);

    // Close write end so read won't block if sendfile failed
    // (macOS sendfile requires a socket destination).
    ::close(pipefd[1]);

    if (awaitable.result_ == static_cast<ssize_t>(data.size())) {
        char buf[128] = {};
        ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        CHECK(std::string(buf, static_cast<std::size_t>(n)) == data);
    }

    ::close(in_fd);
    ::close(pipefd[0]);
    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - sendfile with offset") {
    std::string data = "0123456789ABCDEF";
    auto path = create_temp_file(data);

    int in_fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(in_fd >= 0);

    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    // Send 6 bytes starting at offset 10
    auto awaitable = aio::sendfile(pipefd[1], in_fd, 10, 6);
    CHECK(awaitable.ready_);

    // Close write end so read won't block if sendfile failed.
    ::close(pipefd[1]);

    if (awaitable.result_ == 6) {
        char buf[64] = {};
        ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        CHECK(std::string(buf, static_cast<std::size_t>(n)) == "ABCDEF");
    }

    ::close(in_fd);
    ::close(pipefd[0]);
    ::unlink(path.c_str());
}

// ============================================================================
// Async tests with executor for new ops
// ============================================================================

TEST_CASE("AsyncIO - async readv/writev with executor") {
    char path[] = "/tmp/dftracer_io_async_iov_XXXXXX";
    int tmpfd = ::mkstemp(path);
    REQUIRE(tmpfd >= 0);
    ::close(tmpfd);

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> success{false};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            int fd = ::open(path, O_RDWR | O_TRUNC);
            REQUIRE(fd >= 0);

            // writev
            std::string p1 = "Hello, ";
            std::string p2 = "World!";
            struct iovec wv[2];
            wv[0].iov_base = const_cast<char*>(p1.data());
            wv[0].iov_len = p1.size();
            wv[1].iov_base = const_cast<char*>(p2.data());
            wv[1].iov_len = p2.size();

            ssize_t wn = co_await aio::writev(fd, wv, 2);
            REQUIRE(wn == static_cast<ssize_t>(p1.size() + p2.size()));

            // preadv at offset 0
            char b1[7] = {};
            char b2[6] = {};
            struct iovec rv[2];
            rv[0].iov_base = b1;
            rv[0].iov_len = 7;
            rv[1].iov_base = b2;
            rv[1].iov_len = 6;

            ssize_t rn = co_await aio::preadv(fd, rv, 2, 0);
            if (rn == 13 && std::string(b1, 7) == "Hello, " &&
                std::string(b2, 6) == "World!") {
                success.store(true);
            }

            ::close(fd);
            co_return;
        },
        "AsyncIovTest");

    scheduler.schedule(task);
    task->wait();
    CHECK(success.load());

    executor.shutdown();
    ::unlink(path);
}

TEST_CASE("AsyncIO - async lseek + read with executor") {
    std::string data = "ABCDEFGHIJ";
    auto path = create_temp_file(data);

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    std::atomic<bool> success{false};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            int fd = ::open(path.c_str(), O_RDONLY);
            REQUIRE(fd >= 0);

            ssize_t pos = co_await aio::lseek(fd, 5, SEEK_SET);
            REQUIRE(pos == 5);

            char buf[5] = {};
            ssize_t n = co_await aio::read(fd, buf, 5);
            if (n == 5 && std::string(buf, 5) == "FGHIJ") {
                success.store(true);
            }

            ::close(fd);
            co_return;
        },
        "AsyncLseekTest");

    scheduler.schedule(task);
    task->wait();
    CHECK(success.load());

    executor.shutdown();
    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - async sendfile with executor") {
    std::string data = "async sendfile test data";
    auto path = create_temp_file(data);

    Executor executor(ExecutorConfig{.num_threads = 2});
    Scheduler scheduler(&executor);

    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    std::atomic<bool> success{false};

    auto task = make_task(
        [&](CoroScope& /*scope*/) -> coro::CoroTask<void> {
            int in_fd = ::open(path.c_str(), O_RDONLY);
            REQUIRE(in_fd >= 0);

            ssize_t n =
                co_await aio::sendfile(pipefd[1], in_fd, 0, data.size());
            if (n == static_cast<ssize_t>(data.size())) {
                success.store(true);
            }

            ::close(in_fd);
            co_return;
        },
        "AsyncSendfileTest");

    scheduler.schedule(task);
    task->wait();

    // Close write end so read won't block if sendfile failed
    // (macOS sendfile requires a socket destination, not a pipe).
    ::close(pipefd[1]);

    if (success.load()) {
        char buf[128] = {};
        ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        CHECK(std::string(buf, static_cast<std::size_t>(n)) == data);
    }

    ::close(pipefd[0]);
    executor.shutdown();
    ::unlink(path.c_str());
}