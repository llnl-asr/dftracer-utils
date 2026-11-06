// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/io/ops.h>
#include <dftracer/utils/runtime/io/thread_pool_file_ops.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
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
    TempFile() {
        std::snprintf(path_, sizeof(path_), "/tmp/dftu_io_op_XXXXXX");
        fd_ = ::mkstemp(path_);
        REQUIRE(fd_ >= 0);
    }
    ~TempFile() {
        ::close(fd_);
        ::unlink(path_);
    }
    int fd() const { return fd_; }

   private:
    char path_[64];
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
