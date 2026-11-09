// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <arpa/inet.h>
#include <dftracer/utils/common/scratch.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/io/io_uring_backend.h>
#include <dftracer/utils/runtime/io/ops.h>
#include <dftracer/utils/runtime/io/thread_pool_backend.h>
#include <dftracer/utils/runtime/io/thread_pool_file_ops.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/scheduler/thread_pool_executor.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/xattr.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
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

class TempDir {
   public:
    TempDir() : TempDir(dstd::fs::temp_directory().c_str()) {}
    explicit TempDir(const char* folder) {
        std::snprintf(path_, sizeof(path_), "%s/dftu_io_dir_XXXXXX", folder);
        REQUIRE(::mkdtemp(path_) != nullptr);
    }
    ~TempDir() { dstd::fs::remove_all(dstd::PathView(path_)).discard(); }
    dstd::String join(const char* name) const {
        return dstd::String(path_) + "/" + name;
    }

   private:
    char path_[512];
};

void write_text(const dstd::String& path, const char* text) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    REQUIRE(fd >= 0);
    const auto length = static_cast<ssize_t>(std::strlen(text));
    REQUIRE(::write(fd, text, std::strlen(text)) == length);
    ::close(fd);
}

dstd::String read_text(const dstd::String& path) {
    char buffer[256] = {};
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return dstd::String("<missing>");
    const ssize_t got = ::read(fd, buffer, sizeof(buffer) - 1);
    ::close(fd);
    return got < 0 ? dstd::String("<unreadable>") : dstd::String(buffer);
}

bool is_on_disk(const dstd::String& path) {
    return ::access(path.c_str(), F_OK) == 0;
}

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
    dstd::Result<void> start() override { return {}; }
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
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    // Valgrind before 3.23 keeps its thread lock while a thread waits in
    // io_uring_enter, so a completion that needs another thread never comes.
    return false;
#else
    runtime::io::IoUringBackend backend;
    return backend.probe();
#endif
}
#endif

constexpr runtime::io::IoBackendType TYPES[] = {
    runtime::io::IoBackendType::Threadpool, runtime::io::IoBackendType::Auto};

#ifdef DFTRACER_UTILS_HAVE_IO_URING
constexpr std::uint64_t kind_bit(IoOpKind kind) {
    return std::uint64_t{1} << static_cast<unsigned>(kind);
}

constexpr std::uint64_t FILE_KINDS =
    kind_bit(IoOpKind::Ftruncate) | kind_bit(IoOpKind::Statx) |
    kind_bit(IoOpKind::Unlink) | kind_bit(IoOpKind::Rename) |
    kind_bit(IoOpKind::Mkdir) | kind_bit(IoOpKind::Fallocate) |
    kind_bit(IoOpKind::Fadvise) | kind_bit(IoOpKind::SyncFileRange) |
    kind_bit(IoOpKind::Splice) | kind_bit(IoOpKind::Openat2) |
    kind_bit(IoOpKind::Linkat) | kind_bit(IoOpKind::Symlinkat) |
    kind_bit(IoOpKind::Getxattr) | kind_bit(IoOpKind::Setxattr) |
    kind_bit(IoOpKind::Fgetxattr) | kind_bit(IoOpKind::Fsetxattr) |
    kind_bit(IoOpKind::Socket) | kind_bit(IoOpKind::Connect) |
    kind_bit(IoOpKind::Bind) | kind_bit(IoOpKind::Listen) |
    kind_bit(IoOpKind::Shutdown) | kind_bit(IoOpKind::Sendmsg) |
    kind_bit(IoOpKind::Recvmsg) | kind_bit(IoOpKind::Tee) |
    kind_bit(IoOpKind::Pipe) | kind_bit(IoOpKind::Madvise) |
    kind_bit(IoOpKind::Waitid) | kind_bit(IoOpKind::FutexWait) |
    kind_bit(IoOpKind::FutexWake) | kind_bit(IoOpKind::FutexWaitv) |
    kind_bit(IoOpKind::EpollCtl) | kind_bit(IoOpKind::EpollWait) |
    kind_bit(IoOpKind::PollAdd);

// The ring hides every file op, so each one takes the thread pool.
dstd::UniquePtr<runtime::io::IoBackend> make_pool_for_files(void*) {
    return dstd::make_unique<runtime::io::IoUringBackend>(
        4, runtime::io::IoPoolOptions{}, 0, FILE_KINDS);
}
#endif

// The body runs on each backend, and on a ring that sends the file ops to the
// pool when the kernel has a ring.
template <class Body>
void on_each_backend(Body body) {
    for (const auto type : TYPES) {
        Runtime rt = make_runtime(type);
        body(rt);
    }
#ifdef DFTRACER_UTILS_HAVE_IO_URING
    if (has_ring()) {
        Runtime rt(ExecutorConfig{.thread_count = 4,
                                  .io_backend_factory = &make_pool_for_files},
                   false);
        body(rt);
    } else {
        MESSAGE("skipped the pool run: needs the io_uring backend");
    }
#endif
}

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

namespace {

constexpr std::size_t PAGE_BYTES = 4096;

struct RingCase {
    IoOp op;
    dstd::String from;
    dstd::String to;
    runtime::io::Statx out{};
    alignas(4) std::uint32_t word = 0;
    int fds[2] = {-1, -1};
    int socket_fd = -1;
    void* page = MAP_FAILED;
    std::atomic<bool> done{false};

    RingCase() = default;
    RingCase(const RingCase&) = delete;
    RingCase& operator=(const RingCase&) = delete;
    ~RingCase() {
        for (const int fd : fds) {
            if (fd >= 0) ::close(fd);
        }
        if (socket_fd >= 0) ::close(socket_fd);
        if (page != MAP_FAILED) ::munmap(page, PAGE_BYTES);
    }
};

void ring_case_done(IoOp& op) noexcept {
    static_cast<RingCase*>(op.context)
        ->done.store(true, std::memory_order_release);
}

void fill_ring_case(RingCase& c, IoOpKind kind, const TempDir& dir) {
    c.op.kind = kind;
    c.op.complete = &ring_case_done;
    c.op.context = &c;
    c.from = dir.join("from");
    c.to = dir.join("to");
    switch (kind) {
        case IoOpKind::Statx:
            write_text(c.from, "x");
            c.op.path = c.from.c_str();
            c.op.handle = AT_FDCWD;
            c.op.statx_mask = runtime::io::STATX_BASIC_MASK;
            c.op.statx_out = &c.out;
            break;
        case IoOpKind::Mkdir:
            c.op.path = c.from.c_str();
            c.op.mode = 0755;
            break;
        case IoOpKind::Unlink:
            write_text(c.from, "x");
            c.op.path = c.from.c_str();
            break;
        case IoOpKind::Linkat:
            write_text(c.from, "x");
            c.op.handle = AT_FDCWD;
            c.op.out_handle = AT_FDCWD;
            c.op.path = c.from.c_str();
            c.op.path2 = c.to.c_str();
            break;
        case IoOpKind::Symlinkat:
            c.op.handle = AT_FDCWD;
            c.op.path = c.from.c_str();
            c.op.path2 = c.to.c_str();
            break;
        case IoOpKind::Madvise:
            c.page = ::mmap(nullptr, PAGE_BYTES, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            REQUIRE(c.page != MAP_FAILED);
            c.op.buffer = c.page;
            c.op.length = PAGE_BYTES;
            c.op.flags = MADV_WILLNEED;
            break;
        case IoOpKind::Pipe:
            c.op.pipe_fds = c.fds;
            c.op.flags = O_CLOEXEC;
            break;
        case IoOpKind::FutexWake:
            c.op.futex = &c.word;
            c.op.offset = 1;
            c.op.futex_mask = runtime::io::IO_FUTEX_MATCH_ANY;
            c.op.flags = static_cast<int>(runtime::io::IO_FUTEX_SIZE_U32 |
                                          runtime::io::IO_FUTEX_PRIVATE);
            break;
        case IoOpKind::Listen:
            c.socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            REQUIRE(c.socket_fd >= 0);
            c.op.handle = c.socket_fd;
            c.op.flags = 1;
            break;
        default:
            write_text(c.from, "x");
            c.op.path = c.from.c_str();
            c.op.path2 = c.to.c_str();
            break;
    }
}

struct BusyCall {
    IoOp op;
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    std::atomic<bool> done{false};
};

// Bounded, so a lost release cannot hang the test.
ssize_t hold_pool_thread(void* context) noexcept {
    auto* call = static_cast<BusyCall*>(context);
    call->started.store(true, std::memory_order_release);
    for (int waited = 0;
         waited < 5000 && !call->release.load(std::memory_order_acquire);
         ++waited) {
        ::usleep(1000);
    }
    return 0;
}

void busy_call_done(IoOp& op) noexcept {
    static_cast<BusyCall*>(op.context)
        ->done.store(true, std::memory_order_release);
}

bool wait_true(const std::atomic<bool>& flag, int milliseconds) {
    for (int waited = 0; waited < milliseconds; ++waited) {
        if (flag.load(std::memory_order_acquire)) return true;
        ::usleep(1000);
    }
    return flag.load(std::memory_order_acquire);
}

dstd::UniquePtr<runtime::io::IoBackend> make_one_thread_ring(void* context) {
    return dstd::make_unique<runtime::io::IoUringBackend>(
        1, runtime::io::IoPoolOptions{}, 0,
        *static_cast<std::uint64_t*>(context));
}

// Whether the op ended while the backend's one pool thread was held. The op
// ends in the end either way.
bool ends_while_pool_busy(IoOpKind kind, std::uint64_t masked_kinds,
                          int window_ms, ssize_t& result) {
    Runtime rt(ExecutorConfig{.thread_count = 1,
                              .io_backend_factory = &make_one_thread_ring,
                              .io_backend_context = &masked_kinds},
               false);
    runtime::io::IoBackend& backend = rt.executor()->io_backend();
    TempDir dir;
    RingCase c;
    fill_ring_case(c, kind, dir);
    BusyCall busy;
    busy.op.kind = IoOpKind::Call;
    busy.op.call = &hold_pool_thread;
    busy.op.call_context = &busy;
    busy.op.complete = &busy_call_done;
    busy.op.context = &busy;
    backend.submit(busy.op);
    backend.flush();
    const bool held = wait_true(busy.started, 5000);
    backend.submit(c.op);
    backend.flush();
    const bool ended = held && wait_true(c.done, window_ms);
    busy.release.store(true, std::memory_order_release);
    while (!busy.done.load(std::memory_order_acquire) ||
           !c.done.load(std::memory_order_acquire)) {
        ::usleep(100);
    }
    result = c.op.result;
    return ended;
}

}  // namespace

namespace {

constexpr unsigned SQ_IDLE_MS = 1;

dstd::UniquePtr<runtime::io::IoBackend> make_sq_polled(void* context) {
    return dstd::make_unique<runtime::io::IoUringBackend>(
        4, runtime::io::IoPoolOptions{}, *static_cast<unsigned*>(context), 0U,
        SQ_IDLE_MS);
}

// Sleeps past the idle time each round, so the kernel thread must be woken.
coro::CoroTask<int> run_after_idle(const TempDir& dir, int rounds) {
    const dstd::String file = dir.join("polled");
    const dstd::String sub = dir.join("polled_dir");
    write_text(file, "polled");
    int good = 0;
    for (int i = 0; i < rounds; ++i) {
        ::usleep(20000);
        const ssize_t fd = co_await runtime::io::open(file.c_str(), O_RDONLY);
        char buffer[6] = {};
        const ssize_t got = co_await runtime::io::pread(
            static_cast<int>(fd), buffer, sizeof(buffer), 0);
        const ssize_t closed =
            co_await runtime::io::close(static_cast<int>(fd));
        const ssize_t made = co_await runtime::io::mkdir(sub.c_str());
        const ssize_t removed = co_await runtime::io::rmdir(sub.c_str());
        if (fd >= 0 && got == 6 && closed == 0 && made == 0 && removed == 0) {
            ++good;
        }
    }
    co_return good;
}

coro::CoroTask<void> polled_reader(int fd, std::atomic<int>& good) {
    for (int i = 0; i < 200; ++i) {
        char buffer[6] = {};
        const ssize_t got = co_await runtime::io::pread(fd, buffer, 6, 0);
        if (got == 6) good.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

TEST_CASE("io op: a polled ring runs ops after its kernel thread sleeps") {
    if (!has_ring()) {
        MESSAGE("skipped: needs the io_uring backend");
        return;
    }
    unsigned masked = 0;
    Runtime rt(ExecutorConfig{.thread_count = 2,
                              .io_backend_factory = &make_sq_polled,
                              .io_backend_context = &masked},
               false);
    auto& backend =
        static_cast<runtime::io::IoUringBackend&>(rt.executor()->io_backend());
    if (!backend.is_sq_polled()) {
        MESSAGE("skipped: this kernel refuses SQPOLL");
        return;
    }
    TempDir dir;
    CHECK(rt.block_on(run_after_idle(dir, 5)) == 5);
    // More requests at once than the SQ holds, to reach the wait for SQ room.
    const dstd::String file = dir.join("polled");
    const int fd = ::open(file.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    std::atomic<int> good{0};
    rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            for (int i = 0; i < 400; ++i) scope.spawn(polled_reader(fd, good));
            co_return;
        }));
    ::close(fd);
    CHECK(good.load() == 400 * 200);
}

TEST_CASE(
    "io op: a full SQ on a polled ring waits for room and loses nothing") {
    if (!has_ring()) {
        MESSAGE("skipped: needs the io_uring backend");
        return;
    }
    runtime::io::uring::Ring ring;
    // The backend asks for this feature too, and runs without SQPOLL if absent.
    if (!ring.init(8, IORING_SETUP_SQPOLL, SQ_IDLE_MS) ||
        (ring.features() & IORING_FEAT_SQPOLL_NONFIXED) == 0) {
        MESSAGE(
            "skipped: this kernel has no SQPOLL that the backend would use");
        return;
    }
    unsigned submitted = 0;
    for (int round = 0; round < 50; ++round) {
        // Nothing reaches the kernel before the enter, so the SQ is full.
        unsigned filled = 0;
        while (struct io_uring_sqe* sqe = ring.get_sqe()) {
            runtime::io::uring::prep_nop(sqe);
            ++filled;
        }
        REQUIRE(filled > 0);
        submitted += filled;
        REQUIRE(ring.enter(false) == static_cast<int>(filled));
        // The wait for room ended, so the next round finds a free entry.
        struct io_uring_sqe* next = ring.get_sqe();
        REQUIRE(next != nullptr);
        runtime::io::uring::prep_nop(next);
        ++submitted;
        REQUIRE(ring.enter(false) == 1);
        unsigned reaped = 0;
        while (reaped < submitted) {
            ring.flush_completions();
            if (ring.peek_cqe() == nullptr) {
                ::usleep(100);
                continue;
            }
            ring.cqe_seen();
            ++reaped;
        }
        submitted = 0;
    }
}

TEST_CASE("io op: a kernel without unfixed files on SQPOLL runs without it") {
    if (!has_ring()) {
        MESSAGE("skipped: needs the io_uring backend");
        return;
    }
    unsigned masked = IORING_FEAT_SQPOLL_NONFIXED;
    Runtime rt(ExecutorConfig{.thread_count = 2,
                              .io_backend_factory = &make_sq_polled,
                              .io_backend_context = &masked},
               false);
    auto& backend =
        static_cast<runtime::io::IoUringBackend&>(rt.executor()->io_backend());
    CHECK_FALSE(backend.is_sq_polled());
    TempDir dir;
    CHECK(rt.block_on(run_after_idle(dir, 1)) == 1);
}

TEST_CASE("io op: completions past a full CQ show after a flush") {
    if (!has_ring()) {
        MESSAGE("skipped: needs the io_uring backend");
        return;
    }
    runtime::io::uring::Ring ring;
    REQUIRE(ring.init(2));
    const unsigned cq_entries = ring.cq_entries();
    unsigned accepted = 0;
    while (accepted <= cq_entries) {
        for (int i = 0; i < 2; ++i) {
            struct io_uring_sqe* sqe = ring.get_sqe();
            REQUIRE(sqe != nullptr);
            runtime::io::uring::prep_nop(sqe);
        }
        const int taken = ring.enter(false);
        REQUIRE(taken == 2);
        accepted += static_cast<unsigned>(taken);
    }
    unsigned reaped = 0;
    for (int rounds = 0; rounds < 8 && reaped < accepted; ++rounds) {
        while (ring.peek_cqe() != nullptr) {
            ring.cqe_seen();
            ++reaped;
        }
        ring.flush_completions();
    }
    CHECK(reaped == accepted);
    CHECK(reaped > cq_entries);
}

TEST_CASE("io op: file ops end on the ring while the pool thread is busy") {
    if (!has_ring()) {
        MESSAGE("skipped: needs the io_uring backend");
        return;
    }
    runtime::io::IoUringBackend kernel;
    REQUIRE(kernel.probe());
    constexpr IoOpKind KINDS[] = {IoOpKind::Statx,     IoOpKind::Mkdir,
                                  IoOpKind::Unlink,    IoOpKind::Rename,
                                  IoOpKind::Linkat,    IoOpKind::Symlinkat,
                                  IoOpKind::Madvise,   IoOpKind::Pipe,
                                  IoOpKind::FutexWake, IoOpKind::Listen};
    int tested = 0;
    for (const IoOpKind kind : KINDS) {
        if (!kernel.has_opcode(kind)) continue;
        ++tested;
        ssize_t on_ring = 1;
        CHECK(ends_while_pool_busy(kind, 0, 5000, on_ring));
        CHECK(on_ring == 0);
        // Red check: with the kind hidden it waits for the pool thread.
        ssize_t on_pool = 1;
        CHECK_FALSE(ends_while_pool_busy(kind, kind_bit(kind), 300, on_pool));
        CHECK(on_pool == 0);
    }
    MESSAGE("kinds tested on the ring: " << tested);
    if (!kernel.has_opcode(IoOpKind::Mkdir)) {
        MESSAGE("nothing was proved: the kernel has no mkdirat opcode");
    }
}

TEST_CASE(
    "io op: a masked kind is not a ring opcode, and the others follow the "
    "kernel") {
    if (!has_ring()) {
        MESSAGE("skipped: needs the io_uring backend");
        return;
    }
    runtime::io::IoUringBackend all;
    REQUIRE(all.probe());
    runtime::io::IoUringBackend masked(4, {}, 0,
                                       kind_bit(IoOpKind::Statx) |
                                           kind_bit(IoOpKind::Unlink) |
                                           kind_bit(IoOpKind::PollAdd));
    REQUIRE(masked.probe());
    CHECK_FALSE(masked.has_opcode(IoOpKind::Statx));
    CHECK_FALSE(masked.has_opcode(IoOpKind::Unlink));
    CHECK(all.has_opcode(IoOpKind::PollAdd));
    CHECK_FALSE(masked.has_opcode(IoOpKind::PollAdd));
    CHECK(masked.has_opcode(IoOpKind::Tee) == all.has_opcode(IoOpKind::Tee));
    CHECK(masked.has_opcode(IoOpKind::Mkdir) ==
          all.has_opcode(IoOpKind::Mkdir));
    CHECK(masked.has_opcode(IoOpKind::Pread));
    CHECK_FALSE(masked.has_opcode(IoOpKind::Fstat));
    CHECK_FALSE(masked.has_opcode(IoOpKind::Call));
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

TEST_CASE("io op: unlink removes a file, and a missing one gives -ENOENT") {
    on_each_backend([](Runtime& rt) {
        TempDir dir;
        const dstd::String path = dir.join("file");
        const dstd::String missing = dir.join("missing");
        write_text(path, "bytes");
        ssize_t unlinked = 1;
        ssize_t unlink_missing = 1;
        auto body = [&]() -> coro::CoroTask<void> {
            unlinked = co_await runtime::io::unlink(path.c_str());
            unlink_missing = co_await runtime::io::unlink(missing.c_str());
        };
        rt.block_on(body());
        CHECK(unlinked == 0);
        CHECK_FALSE(is_on_disk(path));
        CHECK(unlink_missing == -ENOENT);
    });
}

TEST_CASE("io op: the renamed file is found under its new name") {
    on_each_backend([](Runtime& rt) {
        TempDir dir;
        const dstd::String from = dir.join("from");
        const dstd::String to = dir.join("to");
        write_text(from, "moved bytes");
        ssize_t renamed = 1;
        auto body = [&]() -> coro::CoroTask<void> {
            renamed = co_await runtime::io::rename(from.c_str(), to.c_str());
        };
        rt.block_on(body());
        CHECK(renamed == 0);
        CHECK_FALSE(is_on_disk(from));
        CHECK(read_text(to) == "moved bytes");
    });
}

TEST_CASE("io op: mkdir makes a directory and rmdir refuses a full one") {
    on_each_backend([](Runtime& rt) {
        TempDir dir;
        const dstd::String sub = dir.join("sub");
        const dstd::String inside = sub + "/file";
        ssize_t made = 1;
        ssize_t made_again = 0;
        ssize_t full = 0;
        ssize_t emptied = 1;
        ssize_t removed = 1;
        bool was_directory = false;
        auto body = [&]() -> coro::CoroTask<void> {
            made = co_await runtime::io::mkdir(sub.c_str(), 0755);
            struct stat info;
            was_directory =
                ::stat(sub.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
            made_again = co_await runtime::io::mkdir(sub.c_str(), 0755);
            write_text(inside, "x");
            full = co_await runtime::io::rmdir(sub.c_str());
            emptied = co_await runtime::io::unlink(inside.c_str());
            removed = co_await runtime::io::rmdir(sub.c_str());
        };
        rt.block_on(body());
        CHECK(made == 0);
        CHECK(was_directory);
        CHECK(made_again == -EEXIST);
        CHECK(full == -ENOTEMPTY);
        CHECK(emptied == 0);
        CHECK(removed == 0);
        CHECK_FALSE(is_on_disk(sub));
    });
}

TEST_CASE("io op: ftruncate changes the size of a file") {
    on_each_backend([](Runtime& rt) {
        TempFile file;
        REQUIRE(::pwrite(file.fd(), "0123456789", 10, 0) == 10);
        ssize_t shrunk = 1;
        ssize_t grown = 1;
        off_t shrunk_size = -1;
        off_t grown_size = -1;
        auto body = [&]() -> coro::CoroTask<void> {
            struct stat info;
            shrunk = co_await runtime::io::ftruncate(file.fd(), 4);
            if (::fstat(file.fd(), &info) == 0) shrunk_size = info.st_size;
            grown = co_await runtime::io::ftruncate(file.fd(), 64);
            if (::fstat(file.fd(), &info) == 0) grown_size = info.st_size;
        };
        rt.block_on(body());
        CHECK(shrunk == 0);
        CHECK(shrunk_size == 4);
        CHECK(grown == 0);
        CHECK(grown_size == 64);
    });
}

TEST_CASE("io op: fadvise gives zero") {
    on_each_backend([](Runtime& rt) {
        TempFile file;
        REQUIRE(::pwrite(file.fd(), "0123456789", 10, 0) == 10);
        ssize_t advised = 1;
        ssize_t advised_long = 1;
        auto body = [&]() -> coro::CoroTask<void> {
            advised = co_await runtime::io::fadvise(file.fd(), 0, 0, 0);
            advised_long = co_await runtime::io::fadvise(
                file.fd(), 0, std::uint64_t{5} << 30, 0);
        };
        rt.block_on(body());
        CHECK(advised == 0);
        CHECK(advised_long == 0);
    });
}

namespace {

// Writes one byte to `fd` from a thread of its own after a delay, so that an
// operation that waits for it really waits.
class DelayedByte {
   public:
    DelayedByte(int fd, int delay_ms)
        : thread_([fd, delay_ms] {
              ::usleep(static_cast<useconds_t>(delay_ms) * 1000);
              const ssize_t put = ::write(fd, "x", 1);
              static_cast<void>(put);
          }) {}
    ~DelayedByte() { thread_.join(); }
    DelayedByte(const DelayedByte&) = delete;
    DelayedByte& operator=(const DelayedByte&) = delete;

   private:
    dstd::Thread thread_;
};

sockaddr_in loopback_address(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    return address;
}

std::uint16_t bound_port(int fd) {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) !=
        0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

}  // namespace

TEST_CASE("io op: linkat makes a second name and symlinkat a link") {
    on_each_backend([](Runtime& rt) {
        TempDir dir;
        const dstd::String from = dir.join("from");
        const dstd::String hard = dir.join("hard");
        const dstd::String soft = dir.join("soft");
        write_text(from, "linked bytes");
        ssize_t linked = 1;
        ssize_t linked_again = 0;
        ssize_t symlinked = 1;
        ssize_t symlinked_again = 0;
        auto body = [&]() -> coro::CoroTask<void> {
            linked = co_await runtime::io::linkat(AT_FDCWD, from.c_str(),
                                                  AT_FDCWD, hard.c_str(), 0);
            linked_again = co_await runtime::io::linkat(
                AT_FDCWD, from.c_str(), AT_FDCWD, hard.c_str(), 0);
            symlinked = co_await runtime::io::symlinkat(from.c_str(), AT_FDCWD,
                                                        soft.c_str());
            symlinked_again = co_await runtime::io::symlinkat(
                from.c_str(), AT_FDCWD, soft.c_str());
        };
        rt.block_on(body());
        CHECK(linked == 0);
        CHECK(linked_again == -EEXIST);
        CHECK(symlinked == 0);
        CHECK(symlinked_again == -EEXIST);
        struct stat first;
        struct stat second;
        REQUIRE(::stat(from.c_str(), &first) == 0);
        REQUIRE(::stat(hard.c_str(), &second) == 0);
        CHECK(first.st_ino == second.st_ino);
        CHECK(first.st_nlink == 2);
        CHECK(read_text(hard) == "linked bytes");
        char target[256] = {};
        REQUIRE(::readlink(soft.c_str(), target, sizeof(target) - 1) > 0);
        CHECK(from == target);
        CHECK(read_text(soft) == "linked bytes");
    });
}

TEST_CASE("io op: madvise gives zero, and -EINVAL for an advice it lacks") {
    on_each_backend([](Runtime& rt) {
        constexpr std::size_t BYTES = 4096;
        void* page = ::mmap(nullptr, BYTES, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
        REQUIRE(page != MAP_FAILED);
        ssize_t advised = 1;
        ssize_t refused = 0;
        auto body = [&]() -> coro::CoroTask<void> {
            advised = co_await runtime::io::madvise(page, BYTES, MADV_WILLNEED);
            refused = co_await runtime::io::madvise(page, BYTES, 9999);
        };
        rt.block_on(body());
        ::munmap(page, BYTES);
        CHECK(advised == 0);
        CHECK(refused == -EINVAL);
    });
}

TEST_CASE("io op: a socket connects, sends, receives and sees a shutdown") {
    on_each_backend([](Runtime& rt) {
        int listener = -1;
        int client = -1;
        int server = -1;
        int duplicate = -1;
        int closed_listener = -1;
        ssize_t bound = 1;
        ssize_t bound_again = 0;
        ssize_t listened = 1;
        ssize_t connected = 1;
        ssize_t refused = 0;
        ssize_t sent = 0;
        ssize_t received = 0;
        ssize_t shut = 1;
        ssize_t eof = -1;
        char text[8] = {};
        auto body = [&]() -> coro::CoroTask<void> {
            const ssize_t listen_fd =
                co_await runtime::io::socket(AF_INET, SOCK_STREAM, 0);
            if (listen_fd < 0) co_return;
            listener = static_cast<int>(listen_fd);
            sockaddr_in any = loopback_address(0);
            bound = co_await runtime::io::bind(
                listener, reinterpret_cast<sockaddr*>(&any), sizeof(any));
            listened = co_await runtime::io::listen(listener, 4);
            sockaddr_in server_address = loopback_address(bound_port(listener));

            const ssize_t dup_fd =
                co_await runtime::io::socket(AF_INET, SOCK_STREAM, 0);
            duplicate = static_cast<int>(dup_fd);
            bound_again = co_await runtime::io::bind(
                duplicate, reinterpret_cast<sockaddr*>(&server_address),
                sizeof(server_address));

            const ssize_t client_fd =
                co_await runtime::io::socket(AF_INET, SOCK_STREAM, 0);
            client = static_cast<int>(client_fd);
            connected = co_await runtime::io::connect(
                client, reinterpret_cast<sockaddr*>(&server_address),
                sizeof(server_address));
            const ssize_t accepted = co_await runtime::io::accept(listener);
            server = static_cast<int>(accepted);

            char ping[] = "ping";
            struct iovec out_vec{ping, 4};
            struct msghdr out_msg{};
            out_msg.msg_iov = &out_vec;
            out_msg.msg_iovlen = 1;
            sent = co_await runtime::io::sendmsg(client, &out_msg);
            struct iovec in_vec{text, sizeof(text) - 1};
            struct msghdr in_msg{};
            in_msg.msg_iov = &in_vec;
            in_msg.msg_iovlen = 1;
            received = co_await runtime::io::recvmsg(server, &in_msg);

            shut = co_await runtime::io::shutdown(client, SHUT_WR);
            struct msghdr end_msg{};
            end_msg.msg_iov = &in_vec;
            end_msg.msg_iovlen = 1;
            eof = co_await runtime::io::recvmsg(server, &end_msg);

            const ssize_t gone_fd =
                co_await runtime::io::socket(AF_INET, SOCK_STREAM, 0);
            closed_listener = static_cast<int>(gone_fd);
            sockaddr_in gone_any = loopback_address(0);
            (void)::bind(closed_listener,
                         reinterpret_cast<sockaddr*>(&gone_any),
                         sizeof(gone_any));
            (void)::listen(closed_listener, 1);
            sockaddr_in gone_address =
                loopback_address(bound_port(closed_listener));
            ::close(closed_listener);
            closed_listener = -1;
            const ssize_t probe_fd =
                co_await runtime::io::socket(AF_INET, SOCK_STREAM, 0);
            refused = co_await runtime::io::connect(
                static_cast<int>(probe_fd),
                reinterpret_cast<sockaddr*>(&gone_address),
                sizeof(gone_address));
            ::close(static_cast<int>(probe_fd));
        };
        rt.block_on(body());
        for (const int fd : {listener, client, server, duplicate}) {
            if (fd >= 0) ::close(fd);
        }
        if (listener < 0) {
            MESSAGE("skipped: no TCP socket in this environment");
            return;
        }
        CHECK(bound == 0);
        CHECK(listened == 0);
        CHECK(bound_again == -EADDRINUSE);
        CHECK(connected == 0);
        CHECK(sent == 4);
        CHECK(received == 4);
        CHECK(std::strcmp(text, "ping") == 0);
        CHECK(shut == 0);
        CHECK(eof == 0);
        CHECK(refused == -ECONNREFUSED);
    });
}

TEST_CASE("io op: poll_add gives the events of a pipe that a writer fills") {
    on_each_backend([](Runtime& rt) {
        int fds[2];
        REQUIRE(::pipe(fds) == 0);
        ssize_t writable = 0;
        ssize_t readable = 0;
        ssize_t bad = 0;
        DelayedByte writer(fds[1], 20);
        auto body = [&]() -> coro::CoroTask<void> {
            writable = co_await runtime::io::poll_add(fds[1], POLLOUT);
            readable = co_await runtime::io::poll_add(fds[0], POLLIN);
            bad = co_await runtime::io::poll_add(-1, POLLIN);
        };
        rt.block_on(body());
        char byte = 0;
        CHECK(::read(fds[0], &byte, 1) == 1);
        ::close(fds[0]);
        ::close(fds[1]);
        CHECK(byte == 'x');
        CHECK((writable & POLLOUT) != 0);
        CHECK((readable & POLLIN) != 0);
        CHECK(bad == -EBADF);
    });
}

TEST_CASE("io op: waitid waits for a forked child and reads its status") {
    on_each_backend([](Runtime& rt) {
        const pid_t child = ::fork();
        if (child == 0) {
            ::usleep(20000);
            ::_exit(7);
        }
        REQUIRE(child > 0);
        siginfo_t info{};
        ssize_t waited = 1;
        ssize_t waited_again = 0;
        auto body = [&]() -> coro::CoroTask<void> {
            waited = co_await runtime::io::waitid(
                P_PID, static_cast<id_t>(child), &info, WEXITED);
            siginfo_t none{};
            waited_again = co_await runtime::io::waitid(
                P_PID, static_cast<id_t>(child), &none, WEXITED);
        };
        rt.block_on(body());
        CHECK(waited == 0);
        CHECK(info.si_pid == child);
        CHECK(info.si_code == CLD_EXITED);
        CHECK(info.si_status == 7);
        CHECK(waited_again == -ECHILD);
    });
}

#ifdef __linux__
TEST_CASE("io op: statx matches stat, and a missing path gives -ENOENT") {
    on_each_backend([](Runtime& rt) {
        TempDir dir;
        const dstd::String path = dir.join("file");
        const dstd::String missing = dir.join("missing");
        write_text(path, "five!");
        REQUIRE(::chmod(path.c_str(), 0640) == 0);
        struct stat expected;
        REQUIRE(::stat(path.c_str(), &expected) == 0);
        runtime::io::Statx found{};
        runtime::io::Statx none{};
        ssize_t found_rc = 1;
        ssize_t missing_rc = 0;
        auto body = [&]() -> coro::CoroTask<void> {
            found_rc = co_await runtime::io::statx(
                AT_FDCWD, path.c_str(), 0, runtime::io::STATX_BASIC_MASK,
                &found);
            missing_rc = co_await runtime::io::statx(
                AT_FDCWD, missing.c_str(), 0, runtime::io::STATX_BASIC_MASK,
                &none);
        };
        rt.block_on(body());
        CHECK(found_rc == 0);
        CHECK(found.size == static_cast<std::uint64_t>(expected.st_size));
        CHECK(found.mode == (expected.st_mode & 0xffffU));
        CHECK(found.ino == static_cast<std::uint64_t>(expected.st_ino));
        CHECK(missing_rc == -ENOENT);
    });
}

TEST_CASE("io op: fallocate grows a file") {
    on_each_backend([](Runtime& rt) {
        TempFile file;
        ssize_t grown = 1;
        auto body = [&]() -> coro::CoroTask<void> {
            grown = co_await runtime::io::fallocate(file.fd(), 0, 0, 8192);
        };
        rt.block_on(body());
        if (grown == -EOPNOTSUPP || grown == -ENOSYS) {
            MESSAGE("skipped: the file system has no fallocate");
            return;
        }
        CHECK(grown == 0);
        struct stat info;
        REQUIRE(::fstat(file.fd(), &info) == 0);
        CHECK(info.st_size == 8192);
    });
}

TEST_CASE("io op: sync_file_range gives zero, and -EBADF for a bad fd") {
    on_each_backend([](Runtime& rt) {
        TempFile file;
        REQUIRE(::pwrite(file.fd(), "0123456789", 10, 0) == 10);
        ssize_t synced = 1;
        ssize_t bad = 0;
        auto body = [&]() -> coro::CoroTask<void> {
            synced = co_await runtime::io::sync_file_range(
                file.fd(), 0, 10, SYNC_FILE_RANGE_WRITE);
            bad = co_await runtime::io::sync_file_range(-1, 0, 10, 0);
        };
        rt.block_on(body());
        if (synced == -EOPNOTSUPP || synced == -ENOSYS) {
            MESSAGE("skipped: the file system has no sync_file_range");
            return;
        }
        CHECK(synced == 0);
        CHECK(bad == -EBADF);
    });
}

TEST_CASE("io op: splice moves the bytes of a file into a pipe") {
    on_each_backend([](Runtime& rt) {
        TempFile file;
        REQUIRE(::pwrite(file.fd(), "splice me", 9, 0) == 9);
        int fds[2];
        REQUIRE(::pipe(fds) == 0);
        ssize_t moved = 0;
        auto body = [&]() -> coro::CoroTask<void> {
            moved =
                co_await runtime::io::splice(file.fd(), 0, fds[1], -1, 9, 0);
        };
        rt.block_on(body());
        char bytes[16] = {};
        const ssize_t got = ::read(fds[0], bytes, sizeof(bytes));
        ::close(fds[0]);
        ::close(fds[1]);
        CHECK(moved == 9);
        CHECK(got == 9);
        CHECK(std::strcmp(bytes, "splice me") == 0);
    });
}

#ifdef __linux__
namespace {

std::atomic<bool> g_cancelled_op_ended{false};

void note_cancelled_op_end(IoOp&) noexcept {
    g_cancelled_op_ended.store(true, std::memory_order_release);
}

void fill_pipe(int write_fd) {
    const int flags = ::fcntl(write_fd, F_GETFL);
    REQUIRE(::fcntl(write_fd, F_SETFL, flags | O_NONBLOCK) == 0);
    char block[4096] = {};
    while (::write(write_fd, block, sizeof(block)) > 0) {
    }
    REQUIRE(::fcntl(write_fd, F_SETFL, flags) == 0);
}

// The op waits on its output end, so only a cancel can end it.
void check_ends_on_cancel(IoOp& op) {
    runtime::io::ThreadPoolBackend pool(1);
    REQUIRE(pool.start().has_value());
    g_cancelled_op_ended.store(false, std::memory_order_release);
    op.complete = &note_cancelled_op_end;
    pool.submit(op);
    pool.flush();
    ::usleep(50 * 1000);
    CHECK_FALSE(g_cancelled_op_ended.load(std::memory_order_acquire));
    pool.cancel_pending();
    for (int i = 0;
         i < 5000 && !g_cancelled_op_ended.load(std::memory_order_acquire);
         ++i) {
        ::usleep(1000);
    }
    REQUIRE(g_cancelled_op_ended.load(std::memory_order_acquire));
    CHECK(op.result == -ECANCELED);
    CHECK(pool.in_flight() == 0);
    pool.stop();
}

}  // namespace

TEST_CASE("io op: a splice into a full pipe ends on cancel on the pool") {
    TempFile file;
    REQUIRE(::pwrite(file.fd(), "x", 1, 0) == 1);
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    fill_pipe(fds[1]);
    IoOp op{};
    op.kind = IoOpKind::Splice;
    op.handle = file.fd();
    op.out_handle = fds[1];
    op.offset = 0;
    op.out_offset = -1;
    op.length = 1;
    check_ends_on_cancel(op);
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_CASE("io op: a tee into a full pipe ends on cancel on the pool") {
    int in[2];
    int out[2];
    REQUIRE(::pipe(in) == 0);
    REQUIRE(::pipe(out) == 0);
    REQUIRE(::write(in[1], "x", 1) == 1);
    fill_pipe(out[1]);
    IoOp op{};
    op.kind = IoOpKind::Tee;
    op.handle = in[0];
    op.out_handle = out[1];
    op.length = 1;
    check_ends_on_cancel(op);
    for (const int fd : {in[0], in[1], out[0], out[1]}) ::close(fd);
}

TEST_CASE("io op: a send to a closed peer gives EPIPE, not a signal") {
    on_each_backend([](Runtime& rt) {
        int fds[2];
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        ::close(fds[1]);
        char byte = 'x';
        struct iovec part{&byte, 1};
        struct msghdr message{};
        message.msg_iov = &part;
        message.msg_iovlen = 1;
        ssize_t sent = 0;
        ssize_t sent_message = 0;
        auto body = [&]() -> coro::CoroTask<void> {
            sent = co_await runtime::io::send(fds[0], &byte, 1, 0);
            sent_message = co_await runtime::io::sendmsg(fds[0], &message, 0);
        };
        rt.block_on(body());
        ::close(fds[0]);
        CHECK(sent == -EPIPE);
        CHECK(sent_message == -EPIPE);
    });
}
#endif

namespace {

// Where a file system keeps extended attributes: a local disk when there is
// one, because tmpfs lacks user attributes before Linux 6.6.
dstd::String attribute_folder() {
    const dstd::Path& disk = common::local_spill_root();
    return dstd::String(disk.empty() ? dstd::fs::temp_directory().c_str()
                                     : disk.c_str());
}

coro::CoroTask<void> futex_waiter(const std::uint32_t* word,
                                  std::atomic<bool>& waiting, ssize_t& result) {
    waiting.store(true, std::memory_order_release);
    result = co_await runtime::io::futex_wait(word, 0);
}

coro::CoroTask<void> futex_waker(std::uint32_t* word,
                                 std::atomic<bool>& waiting, ssize_t& woken) {
    while (!waiting.load(std::memory_order_acquire)) ::usleep(100);
    ::usleep(20000);
    std::atomic_ref<std::uint32_t>(*word).store(1, std::memory_order_release);
    woken = co_await runtime::io::futex_wake(word, 1);
}

coro::CoroTask<void> futexv_waiter(const runtime::io::FutexWaitv* waiters,
                                   std::atomic<bool>& waiting,
                                   ssize_t& result) {
    waiting.store(true, std::memory_order_release);
    result = co_await runtime::io::futex_waitv(waiters, 2);
}

runtime::io::FutexWaitv futex_entry(const std::uint32_t* word) {
    runtime::io::FutexWaitv entry{};
    entry.address = reinterpret_cast<std::uintptr_t>(word);
    entry.flags =
        runtime::io::IO_FUTEX_SIZE_U32 | runtime::io::IO_FUTEX_PRIVATE;
    return entry;
}

constexpr std::uint64_t RESOLVE_NO_SYMLINKS = 0x04;

}  // namespace

TEST_CASE("io op: openat2 opens a file, and its resolve flags refuse a link") {
    on_each_backend([](Runtime& rt) {
        TempDir dir;
        const dstd::String path = dir.join("file");
        const dstd::String link = dir.join("link");
        const dstd::String missing = dir.join("missing");
        write_text(path, "opened");
        REQUIRE(::symlink(path.c_str(), link.c_str()) == 0);
        runtime::io::OpenHow plain{};
        plain.flags = O_RDONLY | O_CLOEXEC;
        runtime::io::OpenHow strict = plain;
        strict.resolve = RESOLVE_NO_SYMLINKS;
        ssize_t opened = -1;
        ssize_t through_link = -1;
        ssize_t refused = 0;
        ssize_t absent = 0;
        auto body = [&]() -> coro::CoroTask<void> {
            opened =
                co_await runtime::io::openat2(AT_FDCWD, path.c_str(), plain);
            through_link =
                co_await runtime::io::openat2(AT_FDCWD, link.c_str(), plain);
            refused =
                co_await runtime::io::openat2(AT_FDCWD, link.c_str(), strict);
            absent =
                co_await runtime::io::openat2(AT_FDCWD, missing.c_str(), plain);
        };
        rt.block_on(body());
        if (opened == -ENOSYS) {
            MESSAGE("skipped: this kernel has no openat2");
            return;
        }
        REQUIRE(opened >= 0);
        char bytes[16] = {};
        CHECK(::read(static_cast<int>(opened), bytes, sizeof(bytes) - 1) == 6);
        CHECK(std::strcmp(bytes, "opened") == 0);
        CHECK((::fcntl(static_cast<int>(opened), F_GETFD) & FD_CLOEXEC) != 0);
        ::close(static_cast<int>(opened));
        REQUIRE(through_link >= 0);
        ::close(static_cast<int>(through_link));
        CHECK(refused == -ELOOP);
        CHECK(absent == -ENOENT);
    });
}

TEST_CASE("io op: an extended attribute is set, read back and missing") {
    on_each_backend([](Runtime& rt) {
        TempDir dir(attribute_folder().c_str());
        const dstd::String path = dir.join("file");
        write_text(path, "x");
        const int fd = ::open(path.c_str(), O_RDWR);
        REQUIRE(fd >= 0);
        char value[] = "by path";
        char fd_value[] = "by fd";
        char by_path[16] = {};
        char by_fd[16] = {};
        char none[16] = {};
        const runtime::io::XattrArgs set_path{"user.dftu_path", value, 7, 0};
        const runtime::io::XattrArgs get_path{"user.dftu_path", by_path,
                                              sizeof(by_path), 0};
        const runtime::io::XattrArgs set_fd{"user.dftu_fd", fd_value, 5, 0};
        const runtime::io::XattrArgs get_fd{"user.dftu_fd", by_fd,
                                            sizeof(by_fd), 0};
        const runtime::io::XattrArgs get_missing{"user.dftu_missing", none,
                                                 sizeof(none), 0};
        const runtime::io::XattrArgs create_again{"user.dftu_path", value, 7,
                                                  XATTR_CREATE};
        ssize_t set_rc = 1;
        ssize_t get_rc = 0;
        ssize_t fset_rc = 1;
        ssize_t fget_rc = 0;
        ssize_t missing_rc = 0;
        ssize_t fmissing_rc = 0;
        ssize_t again_rc = 0;
        auto body = [&]() -> coro::CoroTask<void> {
            set_rc = co_await runtime::io::setxattr(path.c_str(), set_path);
            get_rc = co_await runtime::io::getxattr(path.c_str(), get_path);
            fset_rc = co_await runtime::io::fsetxattr(fd, set_fd);
            fget_rc = co_await runtime::io::fgetxattr(fd, get_fd);
            missing_rc =
                co_await runtime::io::getxattr(path.c_str(), get_missing);
            fmissing_rc = co_await runtime::io::fgetxattr(fd, get_missing);
            again_rc =
                co_await runtime::io::setxattr(path.c_str(), create_again);
        };
        rt.block_on(body());
        ::close(fd);
        if (set_rc == -EOPNOTSUPP || set_rc == -ENOSYS) {
            MESSAGE("skipped: the file system has no user attributes");
            return;
        }
        CHECK(set_rc == 0);
        CHECK(get_rc == 7);
        CHECK(std::memcmp(by_path, "by path", 7) == 0);
        CHECK(fset_rc == 0);
        CHECK(fget_rc == 5);
        CHECK(std::memcmp(by_fd, "by fd", 5) == 0);
        CHECK(missing_rc == -ENODATA);
        CHECK(fmissing_rc == -ENODATA);
        CHECK(again_rc == -EEXIST);
    });
}

TEST_CASE("io op: pipe makes a pipe and tee copies its bytes without taking") {
    on_each_backend([](Runtime& rt) {
        TempFile file;
        int source[2] = {-1, -1};
        int copy[2] = {-1, -1};
        ssize_t made = 1;
        ssize_t made_copy = 1;
        ssize_t put = 0;
        ssize_t teed = 0;
        ssize_t not_a_pipe = 0;
        auto body = [&]() -> coro::CoroTask<void> {
            made = co_await runtime::io::pipe(source, O_CLOEXEC);
            made_copy = co_await runtime::io::pipe(copy, 0);
            if (made != 0 || made_copy != 0) co_return;
            put = ::write(source[1], "tee me", 6);
            teed = co_await runtime::io::tee(source[0], copy[1], 6, 0);
            not_a_pipe = co_await runtime::io::tee(file.fd(), copy[1], 6, 0);
        };
        rt.block_on(body());
        REQUIRE(made == 0);
        REQUIRE(made_copy == 0);
        char first[16] = {};
        char second[16] = {};
        CHECK(put == 6);
        CHECK(teed == 6);
        CHECK(not_a_pipe == -EINVAL);
        CHECK(::read(copy[0], first, sizeof(first) - 1) == 6);
        CHECK(std::strcmp(first, "tee me") == 0);
        CHECK(::read(source[0], second, sizeof(second) - 1) == 6);
        CHECK(std::strcmp(second, "tee me") == 0);
        CHECK((::fcntl(source[0], F_GETFD) & FD_CLOEXEC) != 0);
        CHECK((::fcntl(copy[0], F_GETFD) & FD_CLOEXEC) == 0);
        for (const int fd : {source[0], source[1], copy[0], copy[1]}) {
            ::close(fd);
        }
    });
}

TEST_CASE("io op: a futex wait ends when a futex wake comes") {
    on_each_backend([](Runtime& rt) {
        alignas(4) std::uint32_t other = 9;
        ssize_t mismatch = 0;
        ssize_t empty_wake = -1;
        auto probe = [&]() -> coro::CoroTask<void> {
            mismatch = co_await runtime::io::futex_wait(&other, 0);
            empty_wake = co_await runtime::io::futex_wake(&other, 1);
        };
        rt.block_on(probe());
        if (mismatch == -ENOSYS) {
            MESSAGE("skipped: this kernel has no futex");
            return;
        }
        CHECK(mismatch == -EAGAIN);
        CHECK(empty_wake == 0);

        alignas(4) std::uint32_t word = 0;
        std::atomic<bool> waiting{false};
        ssize_t wait_rc = 1;
        ssize_t woken = -1;
        rt.block_on(
            coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
                scope.spawn(futex_waiter(&word, waiting, wait_rc));
                scope.spawn(futex_waker(&word, waiting, woken));
                co_return;
            }));
        // The wait queues long before the wake. If it did not, the word had
        // changed already, and the wait gave -EAGAIN with no one to wake.
        CHECK((wait_rc == 0 || wait_rc == -EAGAIN));
        CHECK(woken == (wait_rc == 0 ? 1 : 0));
        CHECK(word == 1);
    });
}

TEST_CASE("io op: futex_waitv gives the index of the futex that woke") {
    on_each_backend([](Runtime& rt) {
        alignas(4) std::uint32_t changed[2] = {0, 1};
        const runtime::io::FutexWaitv stale[2] = {futex_entry(&changed[0]),
                                                  futex_entry(&changed[1])};
        ssize_t mismatch = 0;
        auto probe = [&]() -> coro::CoroTask<void> {
            mismatch = co_await runtime::io::futex_waitv(stale, 2);
        };
        rt.block_on(probe());
        if (mismatch == -ENOSYS) {
            MESSAGE("skipped: this kernel has no futex_waitv");
            return;
        }
        CHECK(mismatch == -EAGAIN);

        alignas(4) std::uint32_t words[2] = {0, 0};
        const runtime::io::FutexWaitv waiters[2] = {futex_entry(&words[0]),
                                                    futex_entry(&words[1])};
        std::atomic<bool> waiting{false};
        ssize_t wait_rc = 1;
        ssize_t woken = -1;
        rt.block_on(
            coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
                scope.spawn(futexv_waiter(waiters, waiting, wait_rc));
                scope.spawn(futex_waker(&words[1], waiting, woken));
                co_return;
            }));
        CHECK((wait_rc == 1 || wait_rc == -EAGAIN));
        CHECK(woken == (wait_rc == 1 ? 1 : 0));
    });
}

TEST_CASE("io op: epoll_ctl adds a pipe and epoll_wait sees it ready") {
    on_each_backend([](Runtime& rt) {
        const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        REQUIRE(epoll_fd >= 0);
        int fds[2];
        REQUIRE(::pipe(fds) == 0);
        struct epoll_event interest{};
        interest.events = EPOLLIN;
        interest.data.u64 = 42;
        struct epoll_event ready[2] = {};
        ssize_t added = 1;
        ssize_t added_again = 0;
        ssize_t count = 0;
        ssize_t removed = 1;
        DelayedByte writer(fds[1], 20);
        auto body = [&]() -> coro::CoroTask<void> {
            added = co_await runtime::io::epoll_ctl(epoll_fd, EPOLL_CTL_ADD,
                                                    fds[0], &interest);
            added_again = co_await runtime::io::epoll_ctl(
                epoll_fd, EPOLL_CTL_ADD, fds[0], &interest);
            count = co_await runtime::io::epoll_wait(epoll_fd, ready, 2);
            removed = co_await runtime::io::epoll_ctl(epoll_fd, EPOLL_CTL_DEL,
                                                      fds[0], &interest);
        };
        rt.block_on(body());
        char byte = 0;
        CHECK(::read(fds[0], &byte, 1) == 1);
        ::close(fds[0]);
        ::close(fds[1]);
        ::close(epoll_fd);
        CHECK(added == 0);
        CHECK(added_again == -EEXIST);
        CHECK(count == 1);
        const std::uint64_t ready_data = ready[0].data.u64;
        CHECK(ready_data == 42);
        CHECK((ready[0].events & EPOLLIN) != 0);
        CHECK(removed == 0);
    });
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
