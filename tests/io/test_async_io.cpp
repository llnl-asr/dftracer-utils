// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <task_result.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

using namespace dftracer::utils;

// Helper: create a temp file with known content, return path.
static dstd::String create_temp_file(const dstd::String& content) {
    char path[] = "/tmp/dftracer_io_test_XXXXXX";
    int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);
    ssize_t written = ::write(fd, content.data(), content.size());
    REQUIRE(written == static_cast<ssize_t>(content.size()));
    return dstd::String(path);
}

// ============================================================================
// Sync fallback tests (no executor), sequential runtime::io::read /
// runtime::io::write
// ============================================================================

TEST_CASE("AsyncIO - sync fallback: read without executor") {
    dstd::String data = "Hello, async I/O world!";
    auto path = create_temp_file(data);

    auto fd_file = dstd::File::open(path, {});
    REQUIRE(fd_file);
    const int fd = fd_file->native_handle();

    // No executor -- falls back to synchronous ::read()
    char buf[64] = {};
    auto awaitable = runtime::io::read(fd, buf, data.size());
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == static_cast<ssize_t>(data.size()));
    CHECK(dstd::String(buf, static_cast<std::size_t>(awaitable.result)) ==
          data);

    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - sync fallback: write without executor") {
    char path[] = "/tmp/dftracer_io_write_XXXXXX";
    int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);

    dstd::String data = "write test data";
    auto awaitable = runtime::io::write(fd, data.data(), data.size());
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == static_cast<ssize_t>(data.size()));

    // Read back and verify
    char buf[64] = {};
    ::lseek(fd, 0, SEEK_SET);
    ssize_t n = ::read(fd, buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(data.size()));
    CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == data);

    ::unlink(path);
}

TEST_CASE("AsyncIO - sync fallback: open/close without executor") {
    dstd::String data = "open test";
    auto path = create_temp_file(data);

    auto open_result = runtime::io::open(path.c_str(), O_RDONLY);
    CHECK(open_result.await_ready());
    CHECK(open_result.result >= 0);

    int fd = static_cast<int>(open_result.result);
    auto close_result = runtime::io::close(fd);
    CHECK(close_result.await_ready());
    CHECK(close_result.result == 0);

    ::unlink(path.c_str());
}

// ============================================================================
// Sync fallback tests, positional runtime::io::pread / runtime::io::pwrite
// ============================================================================

TEST_CASE("AsyncIO - sync fallback: pread without executor") {
    dstd::String data = "Hello, positional I/O!";
    auto path = create_temp_file(data);

    auto fd_file = dstd::File::open(path, {});
    REQUIRE(fd_file);
    const int fd = fd_file->native_handle();

    // Read from offset 7
    char buf[64] = {};
    auto awaitable = runtime::io::pread(fd, buf, 15, 7);
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == 15);
    CHECK(dstd::String(buf, 15) == "positional I/O!");

    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - sync fallback: pwrite without executor") {
    char path[] = "/tmp/dftracer_io_pwrite_XXXXXX";
    int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);

    // Write initial content
    dstd::String initial = "AAAAAAAAAA";
    auto wr = ::write(fd, initial.data(), initial.size());
    REQUIRE(wr == static_cast<ssize_t>(initial.size()));

    // Positional write at offset 3
    dstd::String patch = "BBB";
    auto awaitable = runtime::io::pwrite(fd, patch.data(), patch.size(), 3);
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == static_cast<ssize_t>(patch.size()));

    // Read back and verify
    char buf[64] = {};
    ::lseek(fd, 0, SEEK_SET);
    ssize_t n = ::read(fd, buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(initial.size()));
    CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == "AAABBBAAAA");

    ::unlink(path);
}

// ============================================================================
// Key regression test: write to non-seekable fd (pipe)
// ============================================================================

TEST_CASE("AsyncIO - sync fallback: write to pipe (non-seekable fd)") {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    dstd::String data = "pipe write test";

    // runtime::io::write should use ::write() which works on pipes.
    // Previously used ::pwrite() which fails with ESPIPE on pipes.
    auto awaitable = runtime::io::write(pipefd[1], data.data(), data.size());
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == static_cast<ssize_t>(data.size()));

    // Read back from pipe
    char buf[64] = {};
    ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(data.size()));
    CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == data);

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

TEST_CASE("AsyncIO - sync fallback: read from pipe (non-seekable fd)") {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    dstd::String data = "pipe read test";
    ssize_t written = ::write(pipefd[1], data.data(), data.size());
    REQUIRE(written == static_cast<ssize_t>(data.size()));
    ::close(pipefd[1]);  // close write end so read sees EOF after data

    // runtime::io::read should use ::read() which works on pipes.
    char buf[64] = {};
    auto awaitable = runtime::io::read(pipefd[0], buf, data.size());
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == static_cast<ssize_t>(data.size()));
    CHECK(dstd::String(buf, static_cast<std::size_t>(awaitable.result)) ==
          data);

    ::close(pipefd[0]);
}

TEST_CASE("AsyncIO - pwrite fails on pipe (expected ESPIPE)") {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    // pwrite on a pipe should fail with -ESPIPE
    dstd::String data = "should fail";
    auto awaitable =
        runtime::io::pwrite(pipefd[1], data.data(), data.size(), 0);
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == -ESPIPE);

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

TEST_CASE("AsyncIO - pread fails on pipe (expected ESPIPE)") {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    char buf[16] = {};
    auto awaitable = runtime::io::pread(pipefd[0], buf, sizeof(buf), 0);
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == -ESPIPE);

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
    dstd::String part1 = "Hello, ";
    dstd::String part2 = "World!";
    auto a1 = runtime::io::write(fd, part1.data(), part1.size());
    CHECK(a1.await_ready());
    CHECK(a1.result == static_cast<ssize_t>(part1.size()));

    auto a2 = runtime::io::write(fd, part2.data(), part2.size());
    CHECK(a2.await_ready());
    CHECK(a2.result == static_cast<ssize_t>(part2.size()));

    // Read back
    char buf[64] = {};
    ::lseek(fd, 0, SEEK_SET);
    ssize_t n = ::read(fd, buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(part1.size() + part2.size()));
    CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == "Hello, World!");

    ::unlink(path);
}

TEST_CASE("AsyncIO - sequential reads advance file position") {
    dstd::String data = "ABCDEFGHIJ";
    auto path = create_temp_file(data);

    auto fd_file = dstd::File::open(path, {});
    REQUIRE(fd_file);
    const int fd = fd_file->native_handle();

    // Two sequential reads should advance position
    char buf1[5] = {};
    auto a1 = runtime::io::read(fd, buf1, 5);
    CHECK(a1.await_ready());
    CHECK(a1.result == 5);
    CHECK(dstd::String(buf1, 5) == "ABCDE");

    char buf2[5] = {};
    auto a2 = runtime::io::read(fd, buf2, 5);
    CHECK(a2.await_ready());
    CHECK(a2.result == 5);
    CHECK(dstd::String(buf2, 5) == "FGHIJ");

    ::unlink(path.c_str());
}

// ============================================================================
// Positional read/write do NOT change file position
// ============================================================================

TEST_CASE("AsyncIO - pread does not change file position") {
    dstd::String data = "ABCDEFGHIJ";
    auto path = create_temp_file(data);

    auto fd_file = dstd::File::open(path, {});
    REQUIRE(fd_file);
    const int fd = fd_file->native_handle();

    // pread at offset 5
    char buf1[5] = {};
    auto a1 = runtime::io::pread(fd, buf1, 5, 5);
    CHECK(a1.result == 5);
    CHECK(dstd::String(buf1, 5) == "FGHIJ");

    // File position should still be 0, sequential read should get "ABCDE"
    char buf2[5] = {};
    auto a2 = runtime::io::read(fd, buf2, 5);
    CHECK(a2.result == 5);
    CHECK(dstd::String(buf2, 5) == "ABCDE");

    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - pwrite does not change file position") {
    char path[] = "/tmp/dftracer_io_pwrite_pos_XXXXXX";
    int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);

    dstd::String initial = "AAAAAAAAAA";
    auto wr = ::write(fd, initial.data(), initial.size());
    REQUIRE(wr == static_cast<ssize_t>(initial.size()));

    // Seek to position 0
    ::lseek(fd, 0, SEEK_SET);

    // pwrite at offset 5; should NOT change current position
    dstd::String patch = "BBB";
    auto a1 = runtime::io::pwrite(fd, patch.data(), patch.size(), 5);
    CHECK(a1.result == static_cast<ssize_t>(patch.size()));

    // Sequential write at current position (0) should overwrite start
    dstd::String seq = "CC";
    auto a2 = runtime::io::write(fd, seq.data(), seq.size());
    CHECK(a2.result == static_cast<ssize_t>(seq.size()));

    // Read back; expect "CCAAABBBAA"
    char buf[64] = {};
    ::lseek(fd, 0, SEEK_SET);
    ssize_t n = ::read(fd, buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(initial.size()));
    CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == "CCAAABBBAA");

    ::unlink(path);
}

// ============================================================================
// Async tests (with executor + thread pool backend)
// ============================================================================

TEST_CASE("AsyncIO - async read with executor") {
    dstd::String data = "executor async read test data!";
    auto path = create_temp_file(data);

    Runtime rt(2);

    std::atomic<bool> success{false};
    dstd::String read_data;

    auto body = [&]() -> coro::CoroTask<void> {
        auto fd_file = dstd::File::open(path, {});
        REQUIRE(fd_file);
        const int fd = fd_file->native_handle();

        char buf[128] = {};
        ssize_t n = co_await runtime::io::pread(fd, buf, data.size(), 0);
        if (n == static_cast<ssize_t>(data.size())) {
            read_data.assign(buf, static_cast<std::size_t>(n));
            success.store(true);
        }

        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(success.load());
    CHECK(read_data == data);

    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - async write with executor") {
    char path[] = "/tmp/dftracer_io_async_write_XXXXXX";
    int tmpfd = ::mkstemp(path);
    REQUIRE(tmpfd >= 0);
    ::close(tmpfd);

    dstd::String data = "executor async write test!";

    Runtime rt(2);

    std::atomic<bool> success{false};

    auto body = [&]() -> coro::CoroTask<void> {
        auto fd_file = dstd::File::open(
            path, {.access = dstd::Access::Write,
                   .creation = dstd::Creation::TruncateExisting});
        REQUIRE(fd_file);
        const int fd = fd_file->native_handle();

        ssize_t n = co_await runtime::io::write(fd, data.data(), data.size());
        if (n == static_cast<ssize_t>(data.size())) {
            success.store(true);
        }

        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(success.load());

    // Read back and verify
    char buf[128] = {};
    auto rfd_file = dstd::File::open(path, {});
    REQUIRE(rfd_file);
    const int rfd = rfd_file->native_handle();
    ssize_t n = ::read(rfd, buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(data.size()));
    CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == data);

    ::unlink(path);
}

TEST_CASE("AsyncIO - async pread with executor") {
    dstd::String data = "ABCDEFGHIJKLMNOP";
    auto path = create_temp_file(data);

    Runtime rt(2);

    std::atomic<bool> success{false};
    dstd::String read_data;

    auto body = [&]() -> coro::CoroTask<void> {
        auto fd_file = dstd::File::open(path, {});
        REQUIRE(fd_file);
        const int fd = fd_file->native_handle();

        // Read 5 bytes from offset 4
        char buf[16] = {};
        ssize_t n = co_await runtime::io::pread(fd, buf, 5, 4);
        if (n == 5) {
            read_data.assign(buf, 5);
            success.store(true);
        }

        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(success.load());
    CHECK(read_data == "EFGHI");

    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - async pwrite with executor") {
    char path[] = "/tmp/dftracer_io_async_pwrite_XXXXXX";
    int tmpfd = ::mkstemp(path);
    REQUIRE(tmpfd >= 0);
    dstd::String initial = "AAAAAAAAAA";
    auto wr = ::write(tmpfd, initial.data(), initial.size());
    REQUIRE(wr == static_cast<ssize_t>(initial.size()));
    ::close(tmpfd);

    Runtime rt(2);

    std::atomic<bool> success{false};

    auto body = [&]() -> coro::CoroTask<void> {
        auto fd_file = dstd::File::open(path, {.access = dstd::Access::Write});
        REQUIRE(fd_file);
        const int fd = fd_file->native_handle();

        dstd::String patch = "BBB";
        ssize_t n =
            co_await runtime::io::pwrite(fd, patch.data(), patch.size(), 3);
        if (n == static_cast<ssize_t>(patch.size())) {
            success.store(true);
        }

        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(success.load());

    // Read back and verify
    char buf[64] = {};
    auto rfd_file = dstd::File::open(path, {});
    REQUIRE(rfd_file);
    const int rfd = rfd_file->native_handle();
    ssize_t n = ::read(rfd, buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(initial.size()));
    CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == "AAABBBAAAA");

    ::unlink(path);
}

TEST_CASE("AsyncIO - async write to pipe with executor") {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    Runtime rt(2);

    dstd::String data = "async pipe write";
    std::atomic<bool> success{false};

    auto body = [&]() -> coro::CoroTask<void> {
        ssize_t n =
            co_await runtime::io::write(pipefd[1], data.data(), data.size());
        if (n == static_cast<ssize_t>(data.size())) {
            success.store(true);
        }
        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(success.load());

    // Read back from pipe
    char buf[64] = {};
    ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(data.size()));
    CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == data);

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

TEST_CASE("AsyncIO - async open + pread + close lifecycle") {
    dstd::String data = "full lifecycle test";
    auto path = create_temp_file(data);

    Runtime rt(2);

    std::atomic<bool> success{false};
    dstd::String read_data;

    auto body = [&]() -> coro::CoroTask<void> {
        ssize_t fd_result = co_await runtime::io::open(path.c_str(), O_RDONLY);
        REQUIRE(fd_result >= 0);
        int fd = static_cast<int>(fd_result);

        char buf[128] = {};
        ssize_t n = co_await runtime::io::pread(fd, buf, data.size(), 0);
        if (n == static_cast<ssize_t>(data.size())) {
            read_data.assign(buf, static_cast<std::size_t>(n));
        }

        ssize_t close_result = co_await runtime::io::close(fd);
        if (close_result == 0 && n == static_cast<ssize_t>(data.size())) {
            success.store(true);
        }

        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(success.load());
    CHECK(read_data == data);

    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - multiple concurrent async reads") {
    constexpr int N = 8;
    dstd::Vector<dstd::String> paths;
    dstd::Vector<dstd::String> expected;

    for (int i = 0; i < N; ++i) {
        dstd::String content =
            "file " + dstd::to_string(i) + " content that is unique";
        paths.push_back(create_temp_file(content));
        expected.push_back(content);
    }

    Runtime rt(4);

    std::atomic<int> completed{0};
    dstd::Vector<dstd::String> results(N);

    auto body = [&]() -> coro::CoroTask<void> {
        co_await coro::with_scope(
            [&](coro::Scope& scope) -> coro::CoroTask<void> {
                for (int i = 0; i < N; ++i) {
                    auto* path_ptr = &paths[i];
                    auto* expected_ptr = &expected[i];
                    auto* result_ptr = &results[i];
                    auto* completed_ptr = &completed;

                    scope.spawn([path_ptr, expected_ptr, result_ptr,
                                 completed_ptr]() -> coro::CoroTask<void> {
                        auto file = dstd::File::open(*path_ptr, {});
                        if (!file) co_return;
                        const int fd = file->native_handle();

                        char buf[256] = {};
                        ssize_t n = co_await runtime::io::pread(
                            fd, buf, expected_ptr->size(), 0);
                        if (n > 0) {
                            result_ptr->assign(buf,
                                               static_cast<std::size_t>(n));
                        }

                        completed_ptr->fetch_add(1);
                        co_return;
                    });
                }

                co_return;
            });
        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(completed.load() == N);
    for (int i = 0; i < N; ++i) {
        CHECK(results[i] == expected[i]);
    }

    for (auto& p : paths) ::unlink(p.c_str());
}

TEST_CASE("AsyncIO - read error handling (invalid fd)") {
    auto awaitable = runtime::io::read(-1, nullptr, 0);
    // With no executor, sync fallback calls read(-1, ...) which fails
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result < 0);
}

TEST_CASE("AsyncIO - IoBackend::name() reports backend type") {
    Runtime rt(1);

    dstd::String backend_name;

    auto body = [&]() -> coro::CoroTask<void> {
        REQUIRE(coro::Executor::current() != nullptr);
        auto* backend = runtime::io::current_backend();
        REQUIRE(backend != nullptr);
        backend_name = backend->name();
        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(!backend_name.empty());
    MESSAGE("Active I/O backend: ", backend_name);
    CHECK((backend_name == "threadpool" || backend_name == "io_uring" ||
           backend_name == "epoll+threadpool" ||
           backend_name == "kqueue+threadpool"));
}

TEST_CASE("AsyncIO - async read error handling with executor (invalid fd)") {
    Runtime rt(2);

    std::atomic<ssize_t> result{0};

    auto body = [&]() -> coro::CoroTask<void> {
        char buf[16] = {};
        ssize_t n = co_await runtime::io::read(-1, buf, 16);
        result.store(n);
        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(result.load() < 0);
}

TEST_CASE("AsyncIO - large pread with executor") {
    // Write 1MB of data
    constexpr std::size_t SIZE = 1024 * 1024;
    dstd::String data(SIZE, 'A');
    for (std::size_t i = 0; i < SIZE; ++i) {
        data[i] = static_cast<char>('A' + (i % 26));
    }
    auto path = create_temp_file(data);

    Runtime rt(2);

    std::atomic<bool> success{false};

    auto body = [&]() -> coro::CoroTask<void> {
        auto fd_file = dstd::File::open(path, {});
        REQUIRE(fd_file);
        const int fd = fd_file->native_handle();

        dstd::Vector<char> buf(SIZE);
        ssize_t n = co_await runtime::io::pread(fd, buf.data(), SIZE, 0);
        if (n == static_cast<ssize_t>(SIZE) &&
            std::memcmp(buf.data(), data.data(), SIZE) == 0) {
            success.store(true);
        }

        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(success.load());

    ::unlink(path.c_str());
}

// ============================================================================
// Per-backend tests: force each backend and run the full lifecycle
// ============================================================================

/// Helper: run the open+pread+close lifecycle on a forced backend.
static void run_lifecycle_on_backend(runtime::io::IoBackendType type,
                                     const char* type_name) {
    dstd::String data = dstd::String("backend test: ") + type_name;
    auto path = create_temp_file(data);

    Runtime rt(ExecutorConfig{.thread_count = 2, .io_backend_type = type});

    std::atomic<bool> success{false};
    dstd::String read_data;
    dstd::String backend_name;

    auto body = [&]() -> coro::CoroTask<void> {
        REQUIRE(coro::Executor::current() != nullptr);
        auto* backend = runtime::io::current_backend();
        REQUIRE(backend != nullptr);
        backend_name = backend->name();

        ssize_t fd_result = co_await runtime::io::open(path.c_str(), O_RDONLY);
        REQUIRE(fd_result >= 0);
        int fd = static_cast<int>(fd_result);

        char buf[128] = {};
        ssize_t n = co_await runtime::io::pread(fd, buf, data.size(), 0);
        if (n == static_cast<ssize_t>(data.size())) {
            read_data.assign(buf, static_cast<std::size_t>(n));
        }

        ssize_t close_result = co_await runtime::io::close(fd);
        if (close_result == 0 && n == static_cast<ssize_t>(data.size())) {
            success.store(true);
        }

        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    MESSAGE("Backend requested: ", type_name, ", actual: ", backend_name);
    CHECK(success.load());
    CHECK(read_data == data);

    ::unlink(path.c_str());
}

/// Helper: run write to pipe on a forced backend (regression test).
static void run_pipe_write_on_backend(runtime::io::IoBackendType type,
                                      const char* type_name) {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    Runtime rt(ExecutorConfig{.thread_count = 2, .io_backend_type = type});

    dstd::String data = dstd::String("pipe write on ") + type_name;
    std::atomic<bool> success{false};

    auto body = [&]() -> coro::CoroTask<void> {
        ssize_t n =
            co_await runtime::io::write(pipefd[1], data.data(), data.size());
        if (n == static_cast<ssize_t>(data.size())) {
            success.store(true);
        }
        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(success.load());

    // Read back from pipe
    char buf[128] = {};
    ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(data.size()));
    CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == data);

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

TEST_CASE("AsyncIO - lifecycle on threadpool backend (forced)") {
    run_lifecycle_on_backend(runtime::io::IoBackendType::Threadpool,
                             "threadpool");
}

TEST_CASE("AsyncIO - pipe write on threadpool backend (forced)") {
    run_pipe_write_on_backend(runtime::io::IoBackendType::Threadpool,
                              "threadpool");
}

#ifdef __linux__
TEST_CASE("AsyncIO - lifecycle on epoll+threadpool backend (forced)") {
    run_lifecycle_on_backend(runtime::io::IoBackendType::EpollThreadpool,
                             "epoll+threadpool");
}

TEST_CASE("AsyncIO - pipe write on epoll+threadpool backend (forced)") {
    run_pipe_write_on_backend(runtime::io::IoBackendType::EpollThreadpool,
                              "epoll+threadpool");
}
#endif

#ifdef DFTRACER_UTILS_HAVE_IO_URING
TEST_CASE("AsyncIO - lifecycle on io_uring backend (forced)") {
    run_lifecycle_on_backend(runtime::io::IoBackendType::IoUring, "io_uring");
}

TEST_CASE("AsyncIO - pipe write on io_uring backend (forced)") {
    run_pipe_write_on_backend(runtime::io::IoBackendType::IoUring, "io_uring");
}
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
TEST_CASE("AsyncIO - lifecycle on kqueue+threadpool backend (forced)") {
    run_lifecycle_on_backend(runtime::io::IoBackendType::KqueueThreadpool,
                             "kqueue+threadpool");
}

TEST_CASE("AsyncIO - pipe write on kqueue+threadpool backend (forced)") {
    run_pipe_write_on_backend(runtime::io::IoBackendType::KqueueThreadpool,
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

    dstd::String p1 = "Hello, ";
    dstd::String p2 = "World!";
    struct iovec iov[2];
    iov[0].iov_base = const_cast<char*>(p1.data());
    iov[0].iov_len = p1.size();
    iov[1].iov_base = const_cast<char*>(p2.data());
    iov[1].iov_len = p2.size();

    auto awaitable = runtime::io::writev(fd, iov, 2);
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == static_cast<ssize_t>(p1.size() + p2.size()));

    // Read back
    char buf[64] = {};
    ::lseek(fd, 0, SEEK_SET);
    ssize_t n = ::read(fd, buf, sizeof(buf));
    CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == "Hello, World!");

    ::unlink(path);
}

TEST_CASE("AsyncIO - sync fallback: readv without executor") {
    dstd::String data = "ABCDEFGHIJ";
    auto path = create_temp_file(data);

    auto fd_file = dstd::File::open(path, {});
    REQUIRE(fd_file);
    const int fd = fd_file->native_handle();

    char buf1[5] = {};
    char buf2[5] = {};
    struct iovec iov[2];
    iov[0].iov_base = buf1;
    iov[0].iov_len = 5;
    iov[1].iov_base = buf2;
    iov[1].iov_len = 5;

    auto awaitable = runtime::io::readv(fd, iov, 2);
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == 10);
    CHECK(dstd::String(buf1, 5) == "ABCDE");
    CHECK(dstd::String(buf2, 5) == "FGHIJ");

    ::unlink(path.c_str());
}

// ============================================================================
// Scatter-gather positional I/O: preadv / pwritev
// ============================================================================

TEST_CASE("AsyncIO - sync fallback: pwritev without executor") {
    char path[] = "/tmp/dftracer_io_pwritev_XXXXXX";
    int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);

    dstd::String initial = "AAAAAAAAAA";
    auto wr = ::write(fd, initial.data(), initial.size());
    REQUIRE(wr == static_cast<ssize_t>(initial.size()));

    dstd::String p1 = "BB";
    dstd::String p2 = "CC";
    struct iovec iov[2];
    iov[0].iov_base = const_cast<char*>(p1.data());
    iov[0].iov_len = p1.size();
    iov[1].iov_base = const_cast<char*>(p2.data());
    iov[1].iov_len = p2.size();

    auto awaitable = runtime::io::pwritev(fd, iov, 2, 3);
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == 4);

    char buf[64] = {};
    ::lseek(fd, 0, SEEK_SET);
    ssize_t n = ::read(fd, buf, sizeof(buf));
    CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == "AAABBCCAAA");

    ::unlink(path);
}

TEST_CASE("AsyncIO - sync fallback: preadv without executor") {
    dstd::String data = "ABCDEFGHIJ";
    auto path = create_temp_file(data);

    auto fd_file = dstd::File::open(path, {});
    REQUIRE(fd_file);
    const int fd = fd_file->native_handle();

    char buf1[3] = {};
    char buf2[4] = {};
    struct iovec iov[2];
    iov[0].iov_base = buf1;
    iov[0].iov_len = 3;
    iov[1].iov_base = buf2;
    iov[1].iov_len = 4;

    auto awaitable = runtime::io::preadv(fd, iov, 2, 2);
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == 7);
    CHECK(dstd::String(buf1, 3) == "CDE");
    CHECK(dstd::String(buf2, 4) == "FGHI");

    // File position should remain at 0
    char pos_buf[5] = {};
    ssize_t n = ::read(fd, pos_buf, 5);
    CHECK(dstd::String(pos_buf, static_cast<std::size_t>(n)) == "ABCDE");

    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - writev to pipe (non-seekable fd)") {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    dstd::String p1 = "pipe ";
    dstd::String p2 = "scatter";
    struct iovec iov[2];
    iov[0].iov_base = const_cast<char*>(p1.data());
    iov[0].iov_len = p1.size();
    iov[1].iov_base = const_cast<char*>(p2.data());
    iov[1].iov_len = p2.size();

    auto awaitable = runtime::io::writev(pipefd[1], iov, 2);
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == static_cast<ssize_t>(p1.size() + p2.size()));

    char buf[64] = {};
    ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
    CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == "pipe scatter");

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

// ============================================================================
// lseek
// ============================================================================

TEST_CASE("AsyncIO - sync fallback: lseek without executor") {
    dstd::String data = "ABCDEFGHIJ";
    auto path = create_temp_file(data);

    auto fd_file = dstd::File::open(path, {});
    REQUIRE(fd_file);
    const int fd = fd_file->native_handle();

    // Seek to offset 5
    auto awaitable = runtime::io::lseek(fd, 5, SEEK_SET);
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == 5);

    // Sequential read should start from offset 5
    char buf[5] = {};
    ssize_t n = ::read(fd, buf, 5);
    CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == "FGHIJ");

    // Seek relative
    auto a2 = runtime::io::lseek(fd, -3, SEEK_CUR);
    CHECK(a2.await_ready());
    CHECK(a2.result == 7);

    // Seek to end
    auto a3 = runtime::io::lseek(fd, 0, SEEK_END);
    CHECK(a3.await_ready());
    CHECK(a3.result == static_cast<ssize_t>(data.size()));

    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - lseek on pipe fails (expected ESPIPE)") {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    auto awaitable = runtime::io::lseek(pipefd[0], 0, SEEK_SET);
    CHECK(awaitable.await_ready());
    CHECK(awaitable.result == -ESPIPE);

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

// ============================================================================
// sendfile
// ============================================================================

TEST_CASE("AsyncIO - sync fallback: sendfile without executor") {
    dstd::String data = "sendfile test data for zero-copy";
    auto path = create_temp_file(data);

    auto in_fd_file = dstd::File::open(path, {});
    REQUIRE(in_fd_file);
    const int in_fd = in_fd_file->native_handle();

    // Create output pipe to receive the data
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    auto awaitable = runtime::io::sendfile(pipefd[1], in_fd, 0, data.size());
    CHECK(awaitable.await_ready());

    // Close write end so read won't block if sendfile failed
    // (macOS sendfile requires a socket destination).
    ::close(pipefd[1]);

    if (awaitable.result == static_cast<ssize_t>(data.size())) {
        char buf[128] = {};
        ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == data);
    }

    ::close(pipefd[0]);
    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - sendfile with offset") {
    dstd::String data = "0123456789ABCDEF";
    auto path = create_temp_file(data);

    auto in_fd_file = dstd::File::open(path, {});
    REQUIRE(in_fd_file);
    const int in_fd = in_fd_file->native_handle();

    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    // Send 6 bytes starting at offset 10
    auto awaitable = runtime::io::sendfile(pipefd[1], in_fd, 10, 6);
    CHECK(awaitable.await_ready());

    // Close write end so read won't block if sendfile failed.
    ::close(pipefd[1]);

    if (awaitable.result == 6) {
        char buf[64] = {};
        ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == "ABCDEF");
    }

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

    Runtime rt(2);

    std::atomic<bool> success{false};

    auto body = [&]() -> coro::CoroTask<void> {
        auto fd_file = dstd::File::open(
            path, {.access = dstd::Access::ReadWrite,
                   .creation = dstd::Creation::TruncateExisting});
        REQUIRE(fd_file);
        const int fd = fd_file->native_handle();

        // writev
        dstd::String p1 = "Hello, ";
        dstd::String p2 = "World!";
        struct iovec wv[2];
        wv[0].iov_base = const_cast<char*>(p1.data());
        wv[0].iov_len = p1.size();
        wv[1].iov_base = const_cast<char*>(p2.data());
        wv[1].iov_len = p2.size();

        ssize_t wn = co_await runtime::io::writev(fd, wv, 2);
        REQUIRE(wn == static_cast<ssize_t>(p1.size() + p2.size()));

        // preadv at offset 0
        char b1[7] = {};
        char b2[6] = {};
        struct iovec rv[2];
        rv[0].iov_base = b1;
        rv[0].iov_len = 7;
        rv[1].iov_base = b2;
        rv[1].iov_len = 6;

        ssize_t rn = co_await runtime::io::preadv(fd, rv, 2, 0);
        if (rn == 13 && dstd::String(b1, 7) == "Hello, " &&
            dstd::String(b2, 6) == "World!") {
            success.store(true);
        }

        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(success.load());

    ::unlink(path);
}

TEST_CASE("AsyncIO - async lseek + read with executor") {
    dstd::String data = "ABCDEFGHIJ";
    auto path = create_temp_file(data);

    Runtime rt(2);

    std::atomic<bool> success{false};

    auto body = [&]() -> coro::CoroTask<void> {
        auto fd_file = dstd::File::open(path, {});
        REQUIRE(fd_file);
        const int fd = fd_file->native_handle();

        ssize_t pos = co_await runtime::io::lseek(fd, 5, SEEK_SET);
        REQUIRE(pos == 5);

        char buf[5] = {};
        ssize_t n = co_await runtime::io::read(fd, buf, 5);
        if (n == 5 && dstd::String(buf, 5) == "FGHIJ") {
            success.store(true);
        }

        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(success.load());

    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - async sendfile with executor") {
    dstd::String data = "async sendfile test data";
    auto path = create_temp_file(data);

    Runtime rt(2);

    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    std::atomic<bool> success{false};

    auto body = [&]() -> coro::CoroTask<void> {
        auto in_fd_file = dstd::File::open(path, {});
        REQUIRE(in_fd_file);
        const int in_fd = in_fd_file->native_handle();

        ssize_t n =
            co_await runtime::io::sendfile(pipefd[1], in_fd, 0, data.size());
        if (n == static_cast<ssize_t>(data.size())) {
            success.store(true);
        }

        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));

    // Close write end so read won't block if sendfile failed
    // (macOS sendfile requires a socket destination, not a pipe).
    ::close(pipefd[1]);

    if (success.load()) {
        char buf[128] = {};
        ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        CHECK(dstd::String(buf, static_cast<std::size_t>(n)) == data);
    }

    ::close(pipefd[0]);
    ::unlink(path.c_str());
}
TEST_CASE("AsyncIO - open_file opens a file for async pread") {
    dstd::String data = "open_file test data";
    auto path = create_temp_file(data);

    Runtime rt(2);

    dstd::String read_data;

    auto body = [&]() -> coro::CoroTask<void> {
        auto file = co_await runtime::io::open_file(path, {});
        REQUIRE(file);
        char buf[64] = {};
        ssize_t n = co_await runtime::io::pread(file->native_handle(), buf,
                                                data.size(), 0);
        REQUIRE(n == static_cast<ssize_t>(data.size()));
        read_data.assign(buf, static_cast<std::size_t>(n));
        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(read_data == data);

    ::unlink(path.c_str());
}

TEST_CASE("AsyncIO - open_file reports NotFound for a missing file") {
    Runtime rt(2);

    bool is_not_found = false;

    auto body = [&]() -> coro::CoroTask<void> {
        auto file = co_await runtime::io::open_file(
            "/tmp/dftracer_io_open_file_missing_path", {});
        is_not_found =
            !file && file.error().condition() == dstd::Condition::NotFound;
        co_return;
    };

    dftu_utils_test::ok(rt.block_on(body()));
    CHECK(is_not_found);
}
