// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/scratch.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/io/io_uring_backend.h>
#include <dftracer/utils/runtime/io/ops.h>
#include <dftracer/utils/runtime/io/thread_pool_file_ops.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/scheduler/thread_pool_executor.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dftracer::utils;

namespace {

using runtime::io::BlockingFn;
using runtime::io::IoOp;
using runtime::io::IoOpKind;

class TempFile {
   public:
    TempFile() : TempFile(dstd::fs::temp_directory().c_str()) {}
    explicit TempFile(const char* folder) {
        std::snprintf(path_, sizeof(path_), "%s/dftu_io_op_XXXXXX", folder);
        fd_ = ::mkstemp(path_);
        REQUIRE(fd_ >= 0);
    }
    ~TempFile() {
        ::close(fd_);
        ::unlink(path_);
    }
    int fd() const { return fd_; }

   private:
    char path_[512];
    int fd_ = -1;
};

struct Write {
    IoOp op;
    std::atomic<int>* done = nullptr;
    std::atomic<int>* bad = nullptr;
    std::uint32_t value = 0;
};

void write_done(IoOp& op) noexcept {
    auto* write = static_cast<Write*>(op.context);
    if (op.result != static_cast<ssize_t>(sizeof(write->value))) {
        write->bad->fetch_add(1, std::memory_order_relaxed);
    }
    write->done->fetch_add(1, std::memory_order_release);
}

void wait_for(const std::atomic<int>& count, int target) {
    while (count.load(std::memory_order_acquire) != target) ::usleep(100);
}

Runtime make_runtime(runtime::io::IoBackendType type) {
    return Runtime(ExecutorConfig{.thread_count = 4, .io_backend_type = type},
                   false);
}

void check_thread_that_exits(runtime::io::IoBackendType type) {
    Runtime rt = make_runtime(type);
    runtime::io::IoBackend& backend = rt.executor()->io_backend();
    TempFile file;
    constexpr int THREADS = 16;
    constexpr int PER_THREAD = 256;
    dstd::Vector<Write> writes(THREADS * PER_THREAD);
    std::atomic<int> done{0};
    std::atomic<int> bad{0};
    dstd::Vector<dstd::Thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < PER_THREAD; ++i) {
                const int index = t * PER_THREAD + i;
                Write& write = writes[static_cast<std::size_t>(index)];
                write.done = &done;
                write.bad = &bad;
                write.value = static_cast<std::uint32_t>(index);
                write.op.kind = IoOpKind::Pwrite;
                write.op.handle = file.fd();
                write.op.buffer = &write.value;
                write.op.length = sizeof(write.value);
                write.op.offset = static_cast<std::int64_t>(
                    static_cast<std::size_t>(index) * sizeof(write.value));
                write.op.complete = &write_done;
                write.op.context = &write;
                backend.submit(write.op);
            }
            backend.flush();
        });
    }
    for (dstd::Thread& thread : threads) thread.join();
    wait_for(done, THREADS * PER_THREAD);
    CHECK(bad.load() == 0);
    for (int index = 0; index < THREADS * PER_THREAD; ++index) {
        std::uint32_t value = 0;
        const ssize_t got = ::pread(file.fd(), &value, sizeof(value),
                                    static_cast<off_t>(index) * 4);
        REQUIRE(got == static_cast<ssize_t>(sizeof(value)));
        REQUIRE(value == static_cast<std::uint32_t>(index));
    }
}

coro::CoroTask<void> read_one(int fd, std::uint32_t index,
                              std::atomic<int>& bad) {
    std::uint32_t value = 0;
    const ssize_t got = co_await runtime::io::pread(fd, &value, sizeof(value),
                                                    index * sizeof(value));
    if (got != static_cast<ssize_t>(sizeof(value)) || value != index) {
        bad.fetch_add(1, std::memory_order_relaxed);
    }
}

void check_more_ops_than_ring(runtime::io::IoBackendType type) {
    constexpr std::uint32_t OPS = 4000;
    TempFile file;
    for (std::uint32_t i = 0; i < OPS; ++i) {
        REQUIRE(::pwrite(file.fd(), &i, sizeof(i),
                         static_cast<off_t>(i * sizeof(i))) ==
                static_cast<ssize_t>(sizeof(i)));
    }
    Runtime rt = make_runtime(type);
    std::atomic<int> bad{0};
    rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            for (std::uint32_t i = 0; i < OPS; ++i) {
                scope.spawn(read_one(file.fd(), i, bad));
            }
            co_return;
        }));
    CHECK(bad.load() == 0);
}

struct Chunked {
    IoOp op;
    runtime::io::IoBackend* backend = nullptr;
    const char* data = nullptr;
    std::size_t size = 0;
    std::size_t sent = 0;
    std::atomic<int>* done = nullptr;
};

void chunk_done(IoOp& op) noexcept {
    auto* chunked = static_cast<Chunked*>(op.context);
    if (op.result > 0) chunked->sent += static_cast<std::size_t>(op.result);
    if (op.result <= 0 || chunked->sent == chunked->size) {
        chunked->done->store(1, std::memory_order_release);
        return;
    }
    op.buffer = const_cast<char*>(chunked->data) + chunked->sent;
    op.length = 1;
    op.offset = static_cast<std::int64_t>(chunked->sent);
    chunked->backend->submit(op);
    chunked->backend->flush();
}

void check_callback_submits_again(runtime::io::IoBackendType type) {
    Runtime rt = make_runtime(type);
    TempFile file;
    static const char TEXT[] = "one byte at a time";
    std::atomic<int> done{0};
    Chunked chunked;
    chunked.backend = &rt.executor()->io_backend();
    chunked.data = TEXT;
    chunked.size = sizeof(TEXT) - 1;
    chunked.done = &done;
    chunked.op.kind = IoOpKind::Pwrite;
    chunked.op.handle = file.fd();
    chunked.op.buffer = const_cast<char*>(TEXT);
    chunked.op.length = 1;
    chunked.op.complete = &chunk_done;
    chunked.op.context = &chunked;
    chunked.backend->submit(chunked.op);
    chunked.backend->flush();
    wait_for(done, 1);
    CHECK(chunked.sent == chunked.size);
    char read_back[sizeof(TEXT)] = {};
    REQUIRE(::pread(file.fd(), read_back, chunked.size, 0) ==
            static_cast<ssize_t>(chunked.size));
    CHECK(std::strcmp(read_back, TEXT) == 0);
}

ssize_t note_thread(void* context) noexcept {
    *static_cast<dstd::Thread::Id*>(context) = dstd::Thread::current_id();
    return 0;
}

void check_call_runs_off_worker(runtime::io::IoBackendType type) {
    Runtime rt = make_runtime(type);
    dstd::Thread::Id worker{};
    dstd::Thread::Id called{};
    auto body = [&]() -> coro::CoroTask<void> {
        worker = dstd::Thread::current_id();
        BlockingFn fn = &note_thread;
        co_await runtime::io::call(fn, &called);
    };
    rt.block_on(body());
    CHECK(called != worker);
}

struct PeerRead {
    IoOp op;
    char byte = 0;
    std::atomic<int>* done = nullptr;
};

void peer_read_done(IoOp& op) noexcept {
    static_cast<PeerRead*>(op.context)
        ->done->fetch_add(1, std::memory_order_release);
}

// Written only against the public header, apart from the syscall helper.
class CountingBackend final : public runtime::io::IoBackend {
   public:
    explicit CountingBackend(std::atomic<int>& ops) : ops_(ops) {}
    void start() override {}
    void stop() override {}
    void submit(IoOp& op) override {
        ops_.fetch_add(1, std::memory_order_relaxed);
        op.result = runtime::io::run_blocking(op);
        op.complete(op);
    }
    std::size_t poll(int) override { return 0; }
    dstd::String name() const override { return "counting"; }

   private:
    std::atomic<int>& ops_;
};

dstd::UniquePtr<runtime::io::IoBackend> make_counting(void* context) {
    return dstd::make_unique<CountingBackend>(
        *static_cast<std::atomic<int>*>(context));
}

// fsync goes to the kernel's own workers, so it never ends inside the submit.
coro::CoroTask<void> sync_many(int fd, int rounds, std::atomic<int>& bad) {
    std::uint32_t value = 7;
    for (int i = 0; i < rounds; ++i) {
        const ssize_t put =
            co_await runtime::io::pwrite(fd, &value, sizeof(value), 0);
        if (put != static_cast<ssize_t>(sizeof(value))) {
            bad.fetch_add(1, std::memory_order_relaxed);
        }
        const ssize_t synced = co_await runtime::io::fsync(fd);
        if (synced != 0) bad.fetch_add(1, std::memory_order_relaxed);
    }
}

void sync_done(IoOp& op) noexcept {
    static_cast<std::atomic<int>*>(op.context)
        ->fetch_add(op.result == 0 ? 1 : 1000, std::memory_order_release);
}

struct Completion {
    dstd::Thread::Id thread{};
    std::atomic<bool> done{false};
};

void note_done(IoOp& op) noexcept {
    auto* completion = static_cast<Completion*>(op.context);
    completion->thread = dstd::Thread::current_id();
    completion->done.store(true, std::memory_order_release);
}

#ifdef DFTRACER_UTILS_HAVE_IO_URING
// The path of a kernel with no timed enter, whatever this kernel is.
dstd::UniquePtr<runtime::io::IoBackend> make_no_timed_enter(void*) {
    return dstd::make_unique<runtime::io::IoUringBackend>(
        4, runtime::io::IoPoolOptions{}, IORING_FEAT_EXT_ARG);
}

bool has_ring() {
    runtime::io::IoUringBackend backend;
    return backend.probe();
}
#endif

constexpr runtime::io::IoBackendType TYPES[] = {
    runtime::io::IoBackendType::Threadpool, runtime::io::IoBackendType::Auto};

}  // namespace

TEST_CASE("io op: writes outlive the thread that submitted them") {
    for (const auto type : TYPES) check_thread_that_exits(type);
}

TEST_CASE("io op: more operations than the ring holds all end") {
    for (const auto type : TYPES) check_more_ops_than_ring(type);
}

TEST_CASE("io op: a completion may submit the operation again") {
    for (const auto type : TYPES) check_callback_submits_again(type);
}

TEST_CASE("io op: a blocking call leaves the worker") {
    for (const auto type : TYPES) check_call_runs_off_worker(type);
}

TEST_CASE("io op: a user backend from the options runs the operations") {
    std::atomic<int> ops{0};
    TempFile file;
    const char text[] = "abc";
    REQUIRE(::pwrite(file.fd(), text, 3, 0) == 3);
    Runtime rt(ExecutorConfig{.thread_count = 2,
                              .io_backend_factory = &make_counting,
                              .io_backend_context = &ops},
               false);
    CHECK(rt.executor()->io_backend().name() == "counting");
    char read_back[4] = {};
    ssize_t got = 0;
    auto body = [&]() -> coro::CoroTask<void> {
        got = co_await runtime::io::pread(file.fd(), read_back, 3, 0);
    };
    rt.block_on(body());
    CHECK(got == 3);
    CHECK(std::strcmp(read_back, "abc") == 0);
    CHECK(ops.load() == 1);
}

TEST_CASE("io op: a cached read on a worker ends inside its flush") {
    // Not the temp directory, which can be tmpfs: a read there goes to a kernel
    // worker.
    const dstd::Path& disk = common::local_spill_root();
    if (disk.empty()) {
        MESSAGE("skipped: needs a local disk");
        return;
    }
    TempFile file(disk.c_str());
    char data[64] = {};
    REQUIRE(::pwrite(file.fd(), data, sizeof(data), 0) ==
            static_cast<ssize_t>(sizeof(data)));
    Runtime rt(ExecutorConfig{.thread_count = 1}, false);
    runtime::io::IoBackend& backend = rt.executor()->io_backend();
    if (backend.name() != "io_uring") {
        MESSAGE("skipped: needs the io_uring backend");
        return;
    }
    dstd::Thread::Id worker{};
    Completion completed;
    auto body = [&]() -> coro::CoroTask<void> {
        worker = dstd::Thread::current_id();
        IoOp op;
        op.kind = IoOpKind::Pread;
        op.handle = file.fd();
        op.buffer = data;
        op.length = sizeof(data);
        op.complete = &note_done;
        op.context = &completed;
        backend.submit(op);
        backend.flush();
        const bool inside_flush =
            completed.done.load(std::memory_order_acquire);
        // The op lives in this frame, so it must end before the frame does.
        while (!completed.done.load(std::memory_order_acquire)) {
            backend.flush();
            backend.poll(0);
        }
        CHECK(inside_flush);
        CHECK(completed.thread == worker);
        CHECK(op.result == static_cast<ssize_t>(sizeof(data)));
        co_return;
    };
    rt.block_on(body());
}

TEST_CASE("io op: a read on tmpfs gives the right bytes") {
    if (::access("/dev/shm", W_OK) != 0) {
        MESSAGE("skipped: needs a writable /dev/shm");
        return;
    }
    TempFile file("/dev/shm");
    const char text[] = "tmpfs";
    REQUIRE(::pwrite(file.fd(), text, sizeof(text), 0) ==
            static_cast<ssize_t>(sizeof(text)));
    Runtime rt(ExecutorConfig{.thread_count = 2}, false);
    char read_back[sizeof(text)] = {};
    ssize_t got = 0;
    auto body = [&]() -> coro::CoroTask<void> {
        got = co_await runtime::io::pread(file.fd(), read_back,
                                          sizeof(read_back), 0);
    };
    rt.block_on(body());
    CHECK(got == static_cast<ssize_t>(sizeof(text)));
    CHECK(std::strcmp(read_back, text) == 0);
}

TEST_CASE("io op: a parked worker resumes when its own fsync ends") {
    TempFile file;
    std::atomic<int> bad{0};
    Runtime rt(ExecutorConfig{.thread_count = 1}, false);
    rt.block_on(sync_many(file.fd(), 200, bad));
    CHECK(bad.load() == 0);
}

TEST_CASE("io op: a completion arrives while its worker never reaps") {
    // One worker, which spins here without leaving the slice, so only the
    // sysmon can reap its ring.
    TempFile file;
    Runtime rt(ExecutorConfig{.thread_count = 1}, false);
    runtime::io::IoBackend& backend = rt.executor()->io_backend();
    std::atomic<int> done{0};
    bool arrived = false;
    auto body = [&]() -> coro::CoroTask<void> {
        IoOp op;
        op.kind = IoOpKind::Fsync;
        op.handle = file.fd();
        op.complete = &sync_done;
        op.context = &done;
        backend.submit(op);
        backend.flush();
        for (int waited = 0; waited < 20000; ++waited) {
            if (done.load(std::memory_order_acquire) != 0) {
                arrived = true;
                break;
            }
            ::usleep(1000);
        }
        // Past this point the op must have ended, as it lives in this frame.
        while (done.load(std::memory_order_acquire) == 0) ::usleep(1000);
        co_return;
    };
    rt.block_on(body());
    CHECK(arrived);
    CHECK(done.load() == 1);
}

TEST_CASE("io op: a retired worker's writes all end") {
    // A thread's exit makes the kernel cancel what it submitted, and a ring it
    // destroyed never completes anything, so a worker must drain first.
    TempFile file;
    std::atomic<int> bad{0};
    Runtime rt(ExecutorConfig{.thread_count = 8}, false);
    auto* pool = dynamic_cast<ThreadPoolExecutor*>(rt.executor());
    REQUIRE(pool != nullptr);
    std::atomic<bool> stop{false};
    dstd::Thread churn([&] {
        // The newest goes first, so each batch must run long enough for the new
        // workers to take tasks and open rings.
        while (!stop.load(std::memory_order_acquire)) {
            for (int i = 0; i < 6; ++i) pool->add_worker();
            ::usleep(2000);
            for (int i = 0; i < 6; ++i) pool->retire_worker();
        }
    });
    rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            for (int task = 0; task < 64; ++task) {
                scope.spawn(sync_many(file.fd(), 128, bad));
            }
            co_return;
        }));
    stop.store(true, std::memory_order_release);
    churn.join();
    CHECK(bad.load() == 0);
}

TEST_CASE("io op: new work wakes a worker parked on its own ring") {
    Runtime rt(ExecutorConfig{.thread_count = 1}, false);
    runtime::io::IoBackend& backend = rt.executor()->io_backend();
    if (backend.name() != "io_uring") {
        MESSAGE("skipped: needs the io_uring backend");
        return;
    }
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    // Offset -1 reads at the file position, so this waits for the writer and
    // holds the worker's ring in flight for the whole test.
    char byte = 0;
    std::atomic<int> done{0};
    IoOp op;
    op.kind = IoOpKind::Pread;
    op.handle = fds[0];
    op.buffer = &byte;
    op.length = 1;
    op.offset = -1;
    op.complete = &sync_done;
    op.context = &done;
    auto start = [&]() -> coro::CoroTask<void> {
        backend.submit(op);
        backend.flush();
        co_return;
    };
    rt.block_on(start());

    constexpr int ROUNDS = 20;
    int ran = 0;
    const auto began = std::chrono::steady_clock::now();
    for (int i = 0; i < ROUNDS; ++i) {
        ::usleep(2000);
        auto step = [&]() -> coro::CoroTask<void> {
            ++ran;
            co_return;
        };
        rt.block_on(step());
    }
    const auto elapsed = std::chrono::steady_clock::now() - began;
    CHECK(ran == ROUNDS);
    // A lost wake costs each round the park timeout of 256 ms.
    CHECK(elapsed < std::chrono::seconds(2));

    REQUIRE(::write(fds[1], "x", 1) == 1);
    while (done.load(std::memory_order_acquire) == 0) ::usleep(100);
    ::close(fds[0]);
    ::close(fds[1]);
}

#ifdef DFTRACER_UTILS_HAVE_IO_URING
TEST_CASE("io op: a ring that lacks a feature does not pass the probe") {
    if (!has_ring()) {
        MESSAGE("skipped: needs the io_uring backend");
        return;
    }
    runtime::io::IoUringBackend no_position(4, {}, IORING_FEAT_RW_CUR_POS);
    CHECK_FALSE(no_position.probe());
    runtime::io::IoUringBackend no_timed_enter(4, {}, IORING_FEAT_EXT_ARG);
    CHECK(no_timed_enter.probe());
}

TEST_CASE("io op: a timed park ends with nothing done, with no timed enter") {
    if (!has_ring()) {
        MESSAGE("skipped: needs the io_uring backend");
        return;
    }
    Runtime rt(ExecutorConfig{.thread_count = 1,
                              .io_backend_factory = &make_no_timed_enter},
               false);
    runtime::io::IoBackend& backend = rt.executor()->io_backend();
    REQUIRE(backend.name() == "io_uring");
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    char byte = 0;
    std::atomic<int> done{0};
    IoOp op;
    op.kind = IoOpKind::Pread;
    op.handle = fds[0];
    op.buffer = &byte;
    op.length = 1;
    op.offset = -1;
    op.complete = &sync_done;
    op.context = &done;
    auto body = [&]() -> coro::CoroTask<void> {
        backend.submit(op);
        backend.flush();
        // Each round must arm a new timeout, so the last one reaped must not
        // read as an operation.
        for (int round = 0; round < 8; ++round) {
            void* token = backend.prepare_park();
            REQUIRE(token != nullptr);
            const auto began = std::chrono::steady_clock::now();
            backend.park(token, 20);
            const auto elapsed = std::chrono::steady_clock::now() - began;
            backend.finish_park(token);
            // Each timeout expired before the next arms, so none ends it early.
            CHECK(elapsed >= std::chrono::milliseconds(20));
            CHECK(elapsed < std::chrono::seconds(5));
            CHECK(done.load(std::memory_order_acquire) == 0);
        }
        co_return;
    };
    rt.block_on(body());
    REQUIRE(::write(fds[1], "x", 1) == 1);
    while (done.load(std::memory_order_acquire) == 0) ::usleep(100);
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_CASE("io op: a long armed timeout does not hold a shorter park") {
    if (!has_ring()) {
        MESSAGE("skipped: needs the io_uring backend");
        return;
    }
    Runtime rt(ExecutorConfig{.thread_count = 1,
                              .io_backend_factory = &make_no_timed_enter},
               false);
    runtime::io::IoBackend& backend = rt.executor()->io_backend();
    REQUIRE(backend.name() == "io_uring");
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    char byte = 0;
    std::atomic<int> done{0};
    IoOp op;
    op.kind = IoOpKind::Pread;
    op.handle = fds[0];
    op.buffer = &byte;
    op.length = 1;
    op.offset = -1;
    op.complete = &sync_done;
    op.context = &done;
    constexpr auto LONG_MS = 3000;
    auto body = [&]() -> coro::CoroTask<void> {
        backend.submit(op);
        backend.flush();
        // A wake ends this park, which leaves its long timeout armed.
        void* token = backend.prepare_park();
        REQUIRE(token != nullptr);
        backend.unpark(token);
        backend.park(token, LONG_MS);
        backend.finish_park(token);

        token = backend.prepare_park();
        REQUIRE(token != nullptr);
        const auto began = std::chrono::steady_clock::now();
        backend.park(token, 1);
        const auto elapsed = std::chrono::steady_clock::now() - began;
        backend.finish_park(token);
        CHECK(elapsed < std::chrono::milliseconds(LONG_MS / 2));
        co_return;
    };
    rt.block_on(body());
    REQUIRE(::write(fds[1], "x", 1) == 1);
    while (done.load(std::memory_order_acquire) == 0) ::usleep(100);
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_CASE("io op: a parked worker resumes, with no timed enter") {
    if (!has_ring()) {
        MESSAGE("skipped: needs the io_uring backend");
        return;
    }
    TempFile file;
    std::atomic<int> bad{0};
    Runtime rt(ExecutorConfig{.thread_count = 1,
                              .io_backend_factory = &make_no_timed_enter},
               false);
    rt.block_on(sync_many(file.fd(), 200, bad));
    CHECK(bad.load() == 0);
}
#endif

TEST_CASE("io op: a file write passes reads that wait on a peer") {
    // Before the runtime, whose shutdown completes any read a failure leaves.
    constexpr int READS = 1200;
    dstd::Vector<PeerRead> reads(READS);
    std::atomic<int> read_done{0};
    Runtime rt = make_runtime(runtime::io::IoBackendType::Auto);
    runtime::io::IoBackend& backend = rt.executor()->io_backend();
    // The thread pools run a blocked read on a thread, so only the ring can do
    // this with few threads.
    if (backend.name() != "io_uring") return;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    TempFile file;
    // READS is more than both the SQ and the CQ hold.
    for (PeerRead& read : reads) {
        read.done = &read_done;
        read.op.kind = IoOpKind::Read;
        read.op.handle = fds[0];
        read.op.buffer = &read.byte;
        read.op.length = 1;
        read.op.complete = &peer_read_done;
        read.op.context = &read;
        backend.submit(read.op);
    }
    backend.flush();

    std::atomic<int> written{0};
    std::atomic<int> write_bad{0};
    Write write;
    write.done = &written;
    write.bad = &write_bad;
    write.op.kind = IoOpKind::Pwrite;
    write.op.handle = file.fd();
    write.op.buffer = &write.value;
    write.op.length = sizeof(write.value);
    write.op.complete = &write_done;
    write.op.context = &write;
    backend.submit(write.op);
    backend.flush();
    for (int waited = 0; waited < 10000 && written.load() == 0; ++waited) {
        ::usleep(1000);
    }
    CHECK(written.load() == 1);
    CHECK(write_bad.load() == 0);

    dstd::Vector<char> bytes(READS, 'x');
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t put =
            ::write(fds[1], bytes.data() + sent, bytes.size() - sent);
        REQUIRE(put > 0);
        sent += static_cast<std::size_t>(put);
    }
    wait_for(read_done, READS);
    ::close(fds[0]);
    ::close(fds[1]);
}
