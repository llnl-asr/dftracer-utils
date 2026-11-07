// SPDX-License-Identifier: MIT
// The cases follow the behavior that the tests of liburing check,
// https://github.com/axboe/liburing (MIT), Copyright 2020 Jens Axboe.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#ifdef DFTRACER_UTILS_HAVE_IO_URING

#include <arpa/inet.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/io/io_uring_prep.h>
#include <dftracer/utils/runtime/io/io_uring_register.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

using namespace dftracer::utils;
using namespace dftracer::utils::runtime::io::uring;

namespace {

constexpr int WAIT_LIMIT_MS = 5000;

struct Done {
    std::uint64_t user_data;
    std::int32_t res;
    std::uint32_t flags;
};
using Dones = dstd::Vector<Done>;

const Done* find_done(const Dones& all, std::uint64_t user_data) {
    for (const Done& done : all) {
        if (done.user_data == user_data) return &done;
    }
    return nullptr;
}

unsigned done_buffer_id(const Done& done) {
    return done.flags >> IORING_CQE_BUFFER_SHIFT;
}

// Hands the pending SQEs to the kernel and waits, with a limit, until `all`
// holds `count` completions. False when the limit passes first.
bool reap(Ring& ring, std::size_t count, Dones& all,
          int limit_ms = WAIT_LIMIT_MS) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(limit_ms);
    for (;;) {
        ring.flush_completions();
        while (struct io_uring_cqe* cqe = ring.peek_cqe()) {
            all.push_back(Done{cqe->user_data, cqe->res, cqe->flags});
            ring.cqe_seen();
        }
        if (all.size() >= count) return true;
        const std::int64_t left =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count();
        if (left <= 0) return false;
        const int ret = ring.enter_wait(left < 50 ? left : 50);
        if (ret < 0 && ret != -ETIME && ret != -EINTR) return false;
    }
}

struct io_uring_sqe* next_sqe(Ring& ring) {
    struct io_uring_sqe* sqe = ring.get_sqe();
    REQUIRE(sqe != nullptr);
    return sqe;
}

bool kernel_at_least(int major, int minor) {
    struct utsname name;
    if (::uname(&name) != 0) return true;
    int have_major = 0;
    int have_minor = 0;
    if (std::sscanf(name.release, "%d.%d", &have_major, &have_minor) != 2) {
        return true;
    }
    return have_major > major || (have_major == major && have_minor >= minor);
}

// The errors a kernel gives for an opcode or a flag that it does not know.
bool is_unsupported_error(int ret) {
    return ret == -EINVAL || ret == -ENOSYS || ret == -EOPNOTSUPP;
}

// The errors of a feature that needs a device, a build option or privilege.
bool is_environment_error(int ret) {
    return ret == -EOPNOTSUPP || ret == -ENODEV || ret == -ENXIO ||
           ret == -EPERM || ret == -EACCES || ret == -ENOSYS;
}

// True, with a message, when `ret` is an unsupported error on a kernel older
// than the one that has the feature. Any other error is the test's to report.
bool skip_unsupported(int ret, int major, int minor, const char* what) {
    if (ret >= 0 || !is_unsupported_error(ret) ||
        kernel_at_least(major, minor)) {
        return false;
    }
    MESSAGE("skipped: a kernel before " << major << "." << minor << " lacks "
                                        << dstd::String(what) << " (error "
                                        << ret << ")");
    return true;
}

// One 6.8 distribution build refuses every provided buffer ring with EINVAL,
// also from a bare syscall with a zeroed struct. 6.1 and 6.12 accept the same
// call, so only 6.8 may skip.
bool skip_refused_buf_ring(int ret) {
    if (ret != -EINVAL || !kernel_at_least(6, 8) || kernel_at_least(6, 9)) {
        return false;
    }
    MESSAGE("skipped: this 6.8 kernel refuses provided buffer rings");
    return true;
}

bool has_op(Ring& ring, unsigned op, const char* name) {
    Probe probe;
    if (!ring.probe(probe)) {
        MESSAGE("skipped: the kernel has no probe, so " << dstd::String(name)
                                                        << " is unknown");
        return false;
    }
    if (!probe.has(op)) {
        MESSAGE("skipped: the kernel has no " << dstd::String(name));
        return false;
    }
    return true;
}

#define OPEN_RING_OR_SKIP(ring, entries, setup_flags)            \
    do {                                                         \
        RingOptions ring_options_;                               \
        ring_options_.flags = (setup_flags);                     \
        if (!(ring).init((entries), ring_options_)) {            \
            MESSAGE("skipped: io_uring_setup refused the ring"); \
            return;                                              \
        }                                                        \
    } while (0)

#define SKIP_UNLESS_OP(ring, op)                \
    do {                                        \
        if (!has_op((ring), (op), #op)) return; \
    } while (0)

#define SKIP_BEFORE_KERNEL(major, minor, what)                               \
    do {                                                                     \
        if (!kernel_at_least((major), (minor))) {                            \
            MESSAGE("skipped: a kernel before " << (major) << "." << (minor) \
                                                << " lacks " << (what));     \
            return;                                                          \
        }                                                                    \
    } while (0)

// Runs a register call into `var`, skips on an unsupported error of an old
// kernel and requires success otherwise.
#define REGISTER_OR_SKIP(var, call, major, minor, what)    \
    const int var = (call);                                \
    if (skip_unsupported(var, (major), (minor), (what))) { \
        return;                                            \
    }                                                      \
    INFO(what << " gave " << var);                         \
    REQUIRE(var >= 0)

void fill_pattern(char* out, std::size_t size, int seed) {
    for (std::size_t i = 0; i < size; ++i) {
        out[i] =
            static_cast<char>('a' + (static_cast<std::size_t>(seed) + i) % 26);
    }
}

class TempFile {
   public:
    TempFile() {
        std::snprintf(path_, sizeof(path_), "%s/dftu_uring_ring_XXXXXX",
                      dstd::fs::temp_directory().c_str());
        fd_ = ::mkstemp(path_);
        REQUIRE(fd_ >= 0);
    }
    ~TempFile() {
        ::close(fd_);
        ::unlink(path_);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    int fd() const { return fd_; }
    const char* path() const { return path_; }

    // Writes `size` bytes of the pattern `seed` at offset 0.
    void write_pattern(std::size_t size, int seed) {
        char buf[8192];
        REQUIRE(size <= sizeof(buf));
        fill_pattern(buf, size, seed);
        REQUIRE(::pwrite(fd_, buf, size, 0) == static_cast<ssize_t>(size));
    }

   private:
    char path_[512];
    int fd_ = -1;
};

class Pipe {
   public:
    Pipe() { REQUIRE(::pipe2(fds_, O_CLOEXEC) == 0); }
    ~Pipe() {
        ::close(fds_[0]);
        ::close(fds_[1]);
    }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;

    int read_end() const { return fds_[0]; }
    int write_end() const { return fds_[1]; }

   private:
    int fds_[2];
};

class SocketPair {
   public:
    SocketPair() {
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds_) ==
                0);
    }
    ~SocketPair() {
        ::close(fds_[0]);
        ::close(fds_[1]);
    }
    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;

    int first() const { return fds_[0]; }
    int second() const { return fds_[1]; }

   private:
    int fds_[2];
};

// A connected TCP pair on the loopback address.
class TcpPair {
   public:
    TcpPair() = default;
    ~TcpPair() {
        if (client_ >= 0) ::close(client_);
        if (server_ >= 0) ::close(server_);
    }
    TcpPair(const TcpPair&) = delete;
    TcpPair& operator=(const TcpPair&) = delete;

    bool open() {
        const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener < 0) return false;
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(addr);
        bool is_ok =
            ::bind(listener, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr)) == 0 &&
            ::listen(listener, 1) == 0 &&
            ::getsockname(listener, reinterpret_cast<struct sockaddr*>(&addr),
                          &length) == 0;
        if (is_ok) {
            client_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            is_ok =
                client_ >= 0 &&
                ::connect(client_, reinterpret_cast<struct sockaddr*>(&addr),
                          sizeof(addr)) == 0;
        }
        if (is_ok) {
            server_ = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
            is_ok = server_ >= 0;
        }
        ::close(listener);
        return is_ok;
    }

    int client() const { return client_; }
    int server() const { return server_; }

   private:
    int client_ = -1;
    int server_ = -1;
};

bool wait_readable(int fd, int limit_ms) {
    struct pollfd entry;
    entry.fd = fd;
    entry.events = POLLIN;
    entry.revents = 0;
    return ::poll(&entry, 1, limit_ms) == 1 && (entry.revents & POLLIN) != 0;
}

#define OPEN_TCP_OR_SKIP(pair)                                     \
    do {                                                           \
        if (!(pair).open()) {                                      \
            MESSAGE("skipped: no TCP connection on the loopback"); \
            return;                                                \
        }                                                          \
    } while (0)

// Reads from `fd` with the ring at a fixed or ordinary descriptor and checks
// the bytes against the pattern.
void check_read_pattern(Ring& ring, int fd, bool is_fixed, std::size_t size,
                        int seed) {
    char got[256];
    char want[256];
    REQUIRE(size <= sizeof(got));
    std::memset(got, 0, sizeof(got));
    fill_pattern(want, size, seed);
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_read(sqe, fd, got, static_cast<unsigned>(size), 0);
    if (is_fixed) sqe->flags |= IOSQE_FIXED_FILE;
    sqe->user_data = 99;
    Dones done;
    REQUIRE(reap(ring, 1, done));
    REQUIRE(done.size() == 1);
    CHECK(done[0].res == static_cast<std::int32_t>(size));
    CHECK(std::memcmp(got, want, size) == 0);
}

}  // namespace

TEST_CASE("uring register: probe lists the opcodes the kernel has") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    Probe via_ring;
    if (!ring.probe(via_ring)) {
        MESSAGE("skipped: a kernel before 5.6 has no probe");
        return;
    }
    Probe via_call;
    REQUIRE(register_probe(ring, via_call) == 0);
    CHECK(via_call.last_op == via_ring.last_op);
    CHECK(via_call.has(0));
    for (unsigned op = 0; op <= via_call.last_op; ++op) {
        CHECK(via_call.has(op) == via_ring.has(op));
    }
}

TEST_CASE(
    "uring ops: fixed read and write move the bytes of a registered buffer") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SKIP_UNLESS_OP(ring, OP_READ_FIXED);
    SKIP_UNLESS_OP(ring, OP_WRITE_FIXED);
    TempFile file;

    char out[4096];
    char in[4096];
    fill_pattern(out, sizeof(out), 3);
    std::memset(in, 0, sizeof(in));
    struct iovec buffers[2] = {{out, sizeof(out)}, {in, sizeof(in)}};
    REQUIRE(register_buffers(ring, buffers, 2) == 0);

    prep_write_fixed(next_sqe(ring), file.fd(), out, sizeof(out), 0, 0);
    Dones wrote;
    REQUIRE(reap(ring, 1, wrote));
    CHECK(wrote[0].res == static_cast<std::int32_t>(sizeof(out)));
    char check[4096];
    REQUIRE(::pread(file.fd(), check, sizeof(check), 0) ==
            static_cast<ssize_t>(sizeof(check)));
    CHECK(std::memcmp(check, out, sizeof(out)) == 0);

    prep_read_fixed(next_sqe(ring), file.fd(), in, sizeof(in), 0, 1);
    Dones read_done;
    REQUIRE(reap(ring, 1, read_done));
    CHECK(read_done[0].res == static_cast<std::int32_t>(sizeof(in)));
    CHECK(std::memcmp(in, out, sizeof(in)) == 0);

    CHECK(unregister_buffers(ring) == 0);
}

TEST_CASE(
    "uring ops: vectored fixed read and write use slices of a registered "
    "buffer") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SKIP_UNLESS_OP(ring, OP_READV_FIXED);
    SKIP_UNLESS_OP(ring, OP_WRITEV_FIXED);
    TempFile file;

    char out[4096];
    char in[4096];
    fill_pattern(out, sizeof(out), 5);
    std::memset(in, 0, sizeof(in));
    struct iovec buffers[2] = {{out, sizeof(out)}, {in, sizeof(in)}};
    REQUIRE(register_buffers(ring, buffers, 2) == 0);

    struct iovec out_slices[2] = {{out, 1000}, {out + 1000, 3096}};
    prep_writev_fixed(next_sqe(ring), file.fd(), out_slices, 2, 0, 0, 0);
    Dones wrote;
    REQUIRE(reap(ring, 1, wrote));
    CHECK(wrote[0].res == 4096);

    struct iovec in_slices[2] = {{in, 2000}, {in + 2000, 2096}};
    prep_readv_fixed(next_sqe(ring), file.fd(), in_slices, 2, 0, 0, 1);
    Dones read_done;
    REQUIRE(reap(ring, 1, read_done));
    CHECK(read_done[0].res == 4096);
    CHECK(std::memcmp(in, out, sizeof(in)) == 0);
}

TEST_CASE(
    "uring register: a sparse buffer table takes a buffer through "
    "update_buffers") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SKIP_UNLESS_OP(ring, OP_READ_FIXED);
    TempFile file;
    file.write_pattern(512, 7);

    REGISTER_OR_SKIP(sparse,
                     register_buffers2(ring, nullptr, nullptr, 2,
                                       IORING_RSRC_REGISTER_SPARSE),
                     5, 19, "register_buffers2 with the sparse flag");
    CHECK(sparse == 0);
    char in[512];
    std::memset(in, 0, sizeof(in));
    struct iovec buffer = {in, sizeof(in)};
    const int updated = update_buffers(ring, 1, &buffer, nullptr, 1);
    REQUIRE(updated == 1);

    prep_read_fixed(next_sqe(ring), file.fd(), in, sizeof(in), 0, 1);
    Dones read_done;
    REQUIRE(reap(ring, 1, read_done));
    CHECK(read_done[0].res == static_cast<std::int32_t>(sizeof(in)));
    char want[512];
    fill_pattern(want, sizeof(want), 7);
    CHECK(std::memcmp(in, want, sizeof(in)) == 0);
    CHECK(unregister_buffers(ring) == 0);
}

TEST_CASE(
    "uring register: a registered file is read by index and update_files "
    "changes it") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    TempFile first;
    TempFile second;
    first.write_pattern(64, 1);
    second.write_pattern(64, 9);

    const int fds[2] = {first.fd(), first.fd()};
    REQUIRE(register_files(ring, fds, 2) == 0);
    check_read_pattern(ring, 0, true, 64, 1);
    check_read_pattern(ring, 1, true, 64, 1);

    const int replacement = second.fd();
    REQUIRE(update_files(ring, 1, &replacement, 1) == 1);
    check_read_pattern(ring, 0, true, 64, 1);
    check_read_pattern(ring, 1, true, 64, 9);
    CHECK(unregister_files(ring) == 0);
}

TEST_CASE("uring ops: FILES_UPDATE swaps the file behind a fixed index") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SKIP_UNLESS_OP(ring, OP_FILES_UPDATE);
    TempFile first;
    TempFile second;
    first.write_pattern(64, 2);
    second.write_pattern(64, 11);

    const int fds[1] = {first.fd()};
    REQUIRE(register_files(ring, fds, 1) == 0);
    check_read_pattern(ring, 0, true, 64, 2);

    const int replacement[1] = {second.fd()};
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_files_update(sqe, replacement, 1, 0);
    sqe->user_data = 3;
    Dones updated;
    REQUIRE(reap(ring, 1, updated));
    CHECK(updated[0].res == 1);
    check_read_pattern(ring, 0, true, 64, 11);
}

TEST_CASE(
    "uring register: a sparse file table takes a file through update_files2") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    TempFile file;
    file.write_pattern(64, 4);

    REGISTER_OR_SKIP(
        sparse,
        register_files2(ring, nullptr, nullptr, 4, IORING_RSRC_REGISTER_SPARSE),
        5, 19, "register_files2 with the sparse flag");
    CHECK(sparse == 0);

    const int fd = file.fd();
    const int updated = update_files2(ring, 2, &fd, nullptr, 1);
    REQUIRE(updated == 1);
    check_read_pattern(ring, 2, true, 64, 4);

    char got[64];
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_read(sqe, 0, got, sizeof(got), 0);
    sqe->flags |= IOSQE_FIXED_FILE;
    Dones empty;
    REQUIRE(reap(ring, 1, empty));
    CHECK(empty[0].res == -EBADF);
}

TEST_CASE(
    "uring register: an alloc range decides which slot a new descriptor "
    "takes") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    TempFile file;
    file.write_pattern(64, 6);

    REGISTER_OR_SKIP(
        sparse,
        register_files2(ring, nullptr, nullptr, 8, IORING_RSRC_REGISTER_SPARSE),
        5, 19, "register_files2 with the sparse flag");
    CHECK(sparse == 0);
    REGISTER_OR_SKIP(range, register_file_alloc_range(ring, 4, 4), 6, 0,
                     "register_file_alloc_range");
    CHECK(range == 0);

    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_openat(sqe, AT_FDCWD, file.path(), O_RDONLY, 0);
    set_target_fixed_file(sqe, IORING_FILE_INDEX_ALLOC);
    sqe->user_data = 5;
    Dones opened;
    REQUIRE(reap(ring, 1, opened));
    if (opened[0].res == -EINVAL && !kernel_at_least(6, 0)) {
        MESSAGE("skipped: a kernel before 6.0 cannot allocate fixed slots");
        return;
    }
    REQUIRE(opened[0].res >= 4);
    CHECK(opened[0].res < 8);
    check_read_pattern(ring, opened[0].res, true, 64, 6);
}

TEST_CASE(
    "uring ops: FIXED_FD_INSTALL turns a fixed file into a working "
    "descriptor") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SKIP_UNLESS_OP(ring, OP_FIXED_FD_INSTALL);
    TempFile file;
    file.write_pattern(64, 8);

    const int fds[1] = {file.fd()};
    REQUIRE(register_files(ring, fds, 1) == 0);
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_fixed_fd_install(sqe, 0, 0);
    sqe->user_data = 6;
    Dones installed;
    REQUIRE(reap(ring, 1, installed));
    REQUIRE(installed[0].res >= 0);
    const int installed_fd = installed[0].res;
    CHECK(installed_fd != file.fd());

    char got[64];
    char want[64];
    fill_pattern(want, sizeof(want), 8);
    CHECK(::pread(installed_fd, got, sizeof(got), 0) ==
          static_cast<ssize_t>(sizeof(got)));
    CHECK(std::memcmp(got, want, sizeof(got)) == 0);
    ::close(installed_fd);
}

TEST_CASE(
    "uring ops: provided buffers serve a recv and REMOVE_BUFFERS counts what "
    "is left") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SKIP_UNLESS_OP(ring, OP_PROVIDE_BUFFERS);
    SKIP_UNLESS_OP(ring, OP_REMOVE_BUFFERS);
    SocketPair sockets;

    constexpr unsigned GROUP = 7;
    char pool[4][64];
    std::memset(pool, 0, sizeof(pool));
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_provide_buffers(sqe, pool, 64, 4, GROUP, 0);
    sqe->user_data = 1;
    Dones provided;
    REQUIRE(reap(ring, 1, provided));
    REQUIRE(provided[0].res >= 0);

    REQUIRE(::write(sockets.second(), "hello", 5) == 5);
    sqe = next_sqe(ring);
    prep_recv(sqe, sockets.first(), nullptr, 64, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    set_buf_group(sqe, GROUP);
    sqe->user_data = 2;
    Dones received;
    REQUIRE(reap(ring, 1, received));
    REQUIRE(received[0].res == 5);
    REQUIRE((received[0].flags & IORING_CQE_F_BUFFER) != 0);
    const unsigned bid = done_buffer_id(received[0]);
    REQUIRE(bid < 4);
    CHECK(std::memcmp(pool[bid], "hello", 5) == 0);

    sqe = next_sqe(ring);
    prep_remove_buffers(sqe, 4, GROUP);
    sqe->user_data = 3;
    Dones removed;
    REQUIRE(reap(ring, 1, removed));
    CHECK(removed[0].res == 3);
}

TEST_CASE(
    "uring register: a provided buffer ring serves a recv and pbuf_status sees "
    "the head") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SocketPair sockets;

    constexpr unsigned GROUP = 8;
    BufRing buf_ring;
    const int setup = buf_ring.setup(ring, 4, GROUP);
    if (skip_unsupported(setup, 5, 19, "provided buffer rings")) return;
    if (skip_refused_buf_ring(setup)) return;
    REQUIRE(setup == 0);

    char pool[4][64];
    std::memset(pool, 0, sizeof(pool));
    for (unsigned i = 0; i < 4; ++i) buf_ring.add(pool[i], 64, i, i);
    buf_ring.advance(4);

    REQUIRE(::write(sockets.second(), "world", 5) == 5);
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_recv(sqe, sockets.first(), nullptr, 64, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    set_buf_group(sqe, GROUP);
    sqe->user_data = 2;
    Dones received;
    REQUIRE(reap(ring, 1, received));
    REQUIRE(received[0].res == 5);
    REQUIRE((received[0].flags & IORING_CQE_F_BUFFER) != 0);
    const unsigned bid = done_buffer_id(received[0]);
    REQUIRE(bid < 4);
    CHECK(std::memcmp(pool[bid], "world", 5) == 0);

    unsigned head = 0;
    const int status = pbuf_status(ring, GROUP, head);
    if (!skip_unsupported(status, 6, 8, "pbuf_status")) {
        REQUIRE(status == 0);
        CHECK(head == 1);
    }
}

TEST_CASE(
    "uring ops: READ_MULTISHOT posts a CQE per buffer and ends without MORE") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SKIP_UNLESS_OP(ring, OP_READ_MULTISHOT);
    Pipe pipe;

    constexpr unsigned GROUP = 3;
    BufRing buf_ring;
    const int setup = buf_ring.setup(ring, 8, GROUP);
    if (skip_unsupported(setup, 5, 19, "provided buffer rings")) return;
    if (skip_refused_buf_ring(setup)) return;
    REQUIRE(setup == 0);
    char pool[8][4];
    std::memset(pool, 0, sizeof(pool));
    for (unsigned i = 0; i < 8; ++i) buf_ring.add(pool[i], 4, i, i);
    buf_ring.advance(8);

    REQUIRE(::write(pipe.write_end(), "AAAABBBBCCCC", 12) == 12);
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_read_multishot(sqe, pipe.read_end(), 0, 0, GROUP);
    sqe->user_data = 1;
    Dones all;
    REQUIRE(reap(ring, 3, all));
    REQUIRE(all.size() >= 3);
    char joined[13];
    std::memset(joined, 0, sizeof(joined));
    for (std::size_t i = 0; i < 3; ++i) {
        INFO("completion " << i);
        CHECK(all[i].user_data == 1);
        REQUIRE(all[i].res == 4);
        CHECK((all[i].flags & IORING_CQE_F_MORE) != 0);
        REQUIRE((all[i].flags & IORING_CQE_F_BUFFER) != 0);
        REQUIRE(done_buffer_id(all[i]) < 8);
        std::memcpy(joined + i * 4, pool[done_buffer_id(all[i])], 4);
    }
    CHECK(std::strcmp(joined, "AAAABBBBCCCC") == 0);

    sqe = next_sqe(ring);
    prep_cancel(sqe, reinterpret_cast<void*>(static_cast<std::uintptr_t>(1)));
    sqe->user_data = 2;
    REQUIRE(reap(ring, 5, all));
    const Done* cancel = find_done(all, 2);
    REQUIRE(cancel != nullptr);
    CHECK(cancel->res == 0);
    const Done& last = all[all.size() - 1].user_data == 1 ? all[all.size() - 1]
                                                          : all[all.size() - 2];
    CHECK(last.user_data == 1);
    CHECK(last.res == -ECANCELED);
    CHECK((last.flags & IORING_CQE_F_MORE) == 0);
}

TEST_CASE(
    "uring ops: SEND_ZC posts the send with MORE, then a notification, and the "
    "peer gets the bytes") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SKIP_UNLESS_OP(ring, OP_SEND_ZC);
    TcpPair tcp;
    OPEN_TCP_OR_SKIP(tcp);

    char out[1000];
    fill_pattern(out, sizeof(out), 12);
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_send_zc(sqe, tcp.client(), out, sizeof(out), 0, 0);
    sqe->user_data = 1;
    Dones all;
    REQUIRE(reap(ring, 2, all));
    REQUIRE(all.size() == 2);
    CHECK(all[0].user_data == 1);
    CHECK(all[0].res == static_cast<std::int32_t>(sizeof(out)));
    CHECK((all[0].flags & IORING_CQE_F_MORE) != 0);
    CHECK((all[0].flags & IORING_CQE_F_NOTIF) == 0);
    CHECK(all[1].user_data == 1);
    CHECK((all[1].flags & IORING_CQE_F_NOTIF) != 0);
    CHECK((all[1].flags & IORING_CQE_F_MORE) == 0);

    char in[1000];
    std::size_t got = 0;
    while (got < sizeof(in) && wait_readable(tcp.server(), 2000)) {
        const ssize_t n = ::recv(tcp.server(), in + got, sizeof(in) - got, 0);
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    REQUIRE(got == sizeof(in));
    CHECK(std::memcmp(in, out, sizeof(in)) == 0);
}

TEST_CASE(
    "uring ops: SENDMSG_ZC posts the send with MORE, then a notification, and "
    "the peer gets the bytes") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SKIP_UNLESS_OP(ring, OP_SENDMSG_ZC);
    TcpPair tcp;
    OPEN_TCP_OR_SKIP(tcp);

    char out[1000];
    fill_pattern(out, sizeof(out), 14);
    struct iovec vec = {out, sizeof(out)};
    struct msghdr msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &vec;
    msg.msg_iovlen = 1;
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_sendmsg_zc(sqe, tcp.client(), &msg, 0);
    sqe->user_data = 1;
    Dones all;
    REQUIRE(reap(ring, 2, all));
    REQUIRE(all.size() == 2);
    CHECK(all[0].res == static_cast<std::int32_t>(sizeof(out)));
    CHECK((all[0].flags & IORING_CQE_F_MORE) != 0);
    CHECK((all[1].flags & IORING_CQE_F_NOTIF) != 0);

    char in[1000];
    std::size_t got = 0;
    while (got < sizeof(in) && wait_readable(tcp.server(), 2000)) {
        const ssize_t n = ::recv(tcp.server(), in + got, sizeof(in) - got, 0);
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    REQUIRE(got == sizeof(in));
    CHECK(std::memcmp(in, out, sizeof(in)) == 0);
}

TEST_CASE("uring ops: a linked timeout cancels a read of an empty pipe") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SKIP_UNLESS_OP(ring, OP_LINK_TIMEOUT);
    Pipe pipe;

    char buf[8];
    const KernelTimespec limit = {0, 50000000};
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_read(sqe, pipe.read_end(), buf, sizeof(buf), 0);
    sqe->flags |= IOSQE_IO_LINK;
    sqe->user_data = 1;
    sqe = next_sqe(ring);
    prep_link_timeout(sqe, &limit, 0);
    sqe->user_data = 2;
    Dones all;
    REQUIRE(reap(ring, 2, all));
    const Done* read_result = find_done(all, 1);
    const Done* timeout = find_done(all, 2);
    REQUIRE(read_result != nullptr);
    REQUIRE(timeout != nullptr);
    CHECK(read_result->res == -ECANCELED);
    CHECK(timeout->res == -ETIME);
}

TEST_CASE("uring ops: TIMEOUT_REMOVE ends a pending timeout with ECANCELED") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SKIP_UNLESS_OP(ring, OP_TIMEOUT_REMOVE);

    const KernelTimespec far_time = {30, 0};
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_timeout(sqe, &far_time);
    sqe->user_data = 1;
    sqe = next_sqe(ring);
    prep_timeout_remove(sqe, 1, 0);
    sqe->user_data = 2;
    Dones all;
    REQUIRE(reap(ring, 2, all));
    const Done* timeout = find_done(all, 1);
    const Done* remove_result = find_done(all, 2);
    REQUIRE(timeout != nullptr);
    REQUIRE(remove_result != nullptr);
    CHECK(timeout->res == -ECANCELED);
    CHECK(remove_result->res == 0);
}

TEST_CASE(
    "uring ops: TIMEOUT_REMOVE with the update flag moves a pending timeout") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SKIP_UNLESS_OP(ring, OP_TIMEOUT_REMOVE);

    const KernelTimespec far_time = {30, 0};
    const KernelTimespec near_time = {0, 30000000};
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_timeout(sqe, &far_time);
    sqe->user_data = 1;
    REQUIRE(ring.enter(false) == 1);
    sqe = next_sqe(ring);
    prep_timeout_update(sqe, &near_time, 1, 0);
    sqe->user_data = 2;
    Dones all;
    REQUIRE(reap(ring, 2, all));
    const Done* update = find_done(all, 2);
    const Done* timeout = find_done(all, 1);
    REQUIRE(update != nullptr);
    REQUIRE(timeout != nullptr);
    CHECK(update->res == 0);
    CHECK(timeout->res == -ETIME);
}

TEST_CASE("uring ops: POLL_REMOVE ends a pending poll with ECANCELED") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SKIP_UNLESS_OP(ring, OP_POLL_REMOVE);
    Pipe pipe;

    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_poll_add(sqe, pipe.read_end(), POLLIN);
    sqe->user_data = 1;
    sqe = next_sqe(ring);
    prep_poll_remove(sqe, 1);
    sqe->user_data = 2;
    Dones all;
    REQUIRE(reap(ring, 2, all));
    const Done* poll_result = find_done(all, 1);
    const Done* remove_result = find_done(all, 2);
    REQUIRE(poll_result != nullptr);
    REQUIRE(remove_result != nullptr);
    CHECK(poll_result->res == -ECANCELED);
    CHECK(remove_result->res == 0);
}

TEST_CASE(
    "uring ops: MSG_RING posts a CQE with the given user_data and result to "
    "another ring") {
    Ring source;
    Ring target;
    OPEN_RING_OR_SKIP(source, 8, 0);
    OPEN_RING_OR_SKIP(target, 8, 0);
    SKIP_UNLESS_OP(source, OP_MSG_RING);

    struct io_uring_sqe* sqe = next_sqe(source);
    prep_msg_ring(sqe, target.fd(), 42, 0xfeedULL, 0);
    sqe->user_data = 7;
    Dones sent;
    REQUIRE(reap(source, 1, sent));
    CHECK(sent[0].user_data == 7);
    CHECK(sent[0].res == 0);

    Dones arrived;
    REQUIRE(reap(target, 1, arrived));
    CHECK(arrived[0].user_data == 0xfeedULL);
    CHECK(arrived[0].res == 42);
}

TEST_CASE(
    "uring register: send_msg_ring posts a CQE to a ring without a source "
    "ring") {
    Ring target;
    OPEN_RING_OR_SKIP(target, 8, 0);

    struct io_uring_sqe sqe;
    prep_msg_ring(&sqe, target.fd(), 17, 0xbeefULL, 0);
    const int sent = send_msg_ring(sqe);
    if (skip_unsupported(sent, 6, 13, "send_msg_ring")) return;
    REQUIRE(sent == 0);

    Dones arrived;
    REQUIRE(reap(target, 1, arrived));
    CHECK(arrived[0].user_data == 0xbeefULL);
    CHECK(arrived[0].res == 17);
}

TEST_CASE(
    "uring ops: URING_CMD SIOCINQ gives the bytes queued on a TCP socket") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SKIP_UNLESS_OP(ring, OP_URING_CMD);
    SKIP_BEFORE_KERNEL(6, 7, "socket commands in URING_CMD");
    TcpPair tcp;
    OPEN_TCP_OR_SKIP(tcp);

    char out[700];
    fill_pattern(out, sizeof(out), 2);
    REQUIRE(::send(tcp.client(), out, sizeof(out), 0) ==
            static_cast<ssize_t>(sizeof(out)));
    REQUIRE(wait_readable(tcp.server(), 2000));

    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_uring_cmd(sqe, SOCKET_OP_SIOCINQ, tcp.server());
    sqe->user_data = 1;
    Dones all;
    REQUIRE(reap(ring, 1, all));
    CHECK(all[0].res == static_cast<std::int32_t>(sizeof(out)));
}

TEST_CASE(
    "uring register: an eventfd is signaled by a completion until it is "
    "unregistered") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    const int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    REQUIRE(efd >= 0);
    REQUIRE(register_eventfd(ring, efd) == 0);

    prep_nop(next_sqe(ring));
    Dones first;
    REQUIRE(reap(ring, 1, first));
    CHECK(wait_readable(efd, 2000));
    std::uint64_t count = 0;
    CHECK(::read(efd, &count, sizeof(count)) ==
          static_cast<ssize_t>(sizeof(count)));
    CHECK(count >= 1);

    REQUIRE(unregister_eventfd(ring) == 0);
    prep_nop(next_sqe(ring));
    Dones second;
    REQUIRE(reap(ring, 1, second));
    CHECK_FALSE(wait_readable(efd, 100));
    ::close(efd);
}

TEST_CASE(
    "uring register: an async eventfd is signaled only by a completion on a "
    "kernel worker") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    Pipe pipe;
    const int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    REQUIRE(efd >= 0);
    const int registered = register_eventfd_async(ring, efd);
    if (skip_unsupported(registered, 5, 6, "register_eventfd_async")) {
        ::close(efd);
        return;
    }
    REQUIRE(registered == 0);

    prep_nop(next_sqe(ring));
    Dones inline_done;
    REQUIRE(reap(ring, 1, inline_done));
    CHECK_FALSE(wait_readable(efd, 100));

    REQUIRE(::write(pipe.write_end(), "x", 1) == 1);
    char buf[1];
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_read(sqe, pipe.read_end(), buf, sizeof(buf), 0);
    sqe->flags |= IOSQE_ASYNC;
    Dones worker_done;
    REQUIRE(reap(ring, 1, worker_done));
    CHECK(worker_done[0].res == 1);
    CHECK(wait_readable(efd, 2000));
    ::close(efd);
}

TEST_CASE(
    "uring register: a personality is an id that requests use until it is "
    "unregistered") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    REGISTER_OR_SKIP(id, register_personality(ring), 5, 6,
                     "register_personality");
    CHECK(id > 0);

    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_nop(sqe);
    set_personality(sqe, static_cast<unsigned>(id));
    sqe->user_data = 1;
    Dones used;
    REQUIRE(reap(ring, 1, used));
    CHECK(used[0].res == 0);

    REQUIRE(unregister_personality(ring, id) == 0);
    sqe = next_sqe(ring);
    prep_nop(sqe);
    set_personality(sqe, static_cast<unsigned>(id));
    sqe->user_data = 2;
    Dones gone;
    REQUIRE(reap(ring, 1, gone));
    CHECK(gone[0].res == -EINVAL);
}

TEST_CASE(
    "uring register: restrictions on a disabled ring refuse a forbidden op "
    "after enable") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, IORING_SETUP_R_DISABLED);
    TempFile file;

    Restriction allow_nop;
    allow_nop.opcode = RESTRICTION_SQE_OP;
    allow_nop.value = 0;
    REGISTER_OR_SKIP(restricted, register_restrictions(ring, &allow_nop, 1), 5,
                     10, "register_restrictions");
    CHECK(restricted == 0);
    REQUIRE(enable_rings(ring) == 0);

    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_nop(sqe);
    sqe->user_data = 1;
    sqe = next_sqe(ring);
    prep_fsync(sqe, file.fd(), 0);
    sqe->user_data = 2;
    Dones all;
    REQUIRE(reap(ring, 2, all));
    const Done* nop = find_done(all, 1);
    const Done* fsync_result = find_done(all, 2);
    REQUIRE(nop != nullptr);
    REQUIRE(fsync_result != nullptr);
    CHECK(nop->res == 0);
    CHECK(fsync_result->res == -EACCES);
}

TEST_CASE(
    "uring register: iowq max workers returns the old limits and keeps the new "
    "ones") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);

    unsigned original[2] = {0, 0};
    REGISTER_OR_SKIP(read_first, register_iowq_max_workers(ring, original), 5,
                     15, "register_iowq_max_workers");
    CHECK(read_first == 0);
    CHECK(original[0] > 0);
    CHECK(original[1] > 0);

    unsigned set[2] = {1, 2};
    REQUIRE(register_iowq_max_workers(ring, set) == 0);
    CHECK(set[0] == original[0]);
    CHECK(set[1] == original[1]);

    unsigned now[2] = {0, 0};
    REQUIRE(register_iowq_max_workers(ring, now) == 0);
    CHECK(now[0] == 1);
    CHECK(now[1] == 2);

    unsigned restore[2] = {original[0], original[1]};
    CHECK(register_iowq_max_workers(ring, restore) == 0);
}

namespace {

// The kernel workers of a task are named iou-wrk-<tid of the task>, and the
// io-wq belongs to the task, not to a ring. True when this process has one for
// `owner` and gives its thread id.
bool find_iowq_worker(pid_t owner, pid_t& worker) {
    char want[32];
    std::snprintf(want, sizeof(want), "iou-wrk-%d\n", static_cast<int>(owner));
    DIR* dir = ::opendir("/proc/self/task");
    if (dir == nullptr) return false;
    bool is_found = false;
    while (struct dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char path[320];
        std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm",
                      entry->d_name);
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char comm[64];
        const ssize_t n = ::read(fd, comm, sizeof(comm) - 1);
        ::close(fd);
        if (n <= 0) continue;
        comm[n] = '\0';
        if (std::strcmp(comm, want) == 0) {
            worker = static_cast<pid_t>(std::atoi(entry->d_name));
            is_found = true;
            break;
        }
    }
    ::closedir(dir);
    return is_found;
}

// What the affinity case saw on its own thread. Assertions wait for the join.
struct AffinityResult {
    bool is_ring_open = false;
    int pinned = 0;
    int cpu = -1;
    bool is_worker_found = false;
    int worker_cpus = 0;
    bool is_worker_on_cpu = false;
    bool is_read_done = false;
    int unpinned = 0;
};

// Runs on a thread of its own: the io-wq is made for the task that creates the
// ring, and a worker that an earlier case left on the test thread would keep
// the mask it had.
void run_affinity_case(AffinityResult& result) {
    Ring ring;
    RingOptions options;
    if (!ring.init(8, options)) return;
    result.is_ring_open = true;

    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (::sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return;
    for (int i = 0; i < CPU_SETSIZE && result.cpu < 0; ++i) {
        if (CPU_ISSET(i, &allowed)) result.cpu = i;
    }
    if (result.cpu < 0) return;
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(result.cpu, &one);
    result.pinned = register_iowq_aff(ring, &one, sizeof(one));
    if (result.pinned < 0) return;

    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) return;
    char buf[1];
    struct io_uring_sqe* sqe = ring.get_sqe();
    if (sqe != nullptr) {
        prep_read(sqe, fds[0], buf, sizeof(buf), 0);
        sqe->flags |= IOSQE_ASYNC;
        sqe->user_data = 1;
        if (ring.enter(false) == 1) {
            const pid_t owner = static_cast<pid_t>(::syscall(SYS_gettid));
            pid_t worker = -1;
            for (int waited = 0; waited < 2000 && !result.is_worker_found;
                 waited += 10) {
                result.is_worker_found = find_iowq_worker(owner, worker);
                if (!result.is_worker_found) ::usleep(10000);
            }
            if (result.is_worker_found) {
                cpu_set_t mask;
                CPU_ZERO(&mask);
                if (::sched_getaffinity(worker, sizeof(mask), &mask) == 0) {
                    result.worker_cpus = CPU_COUNT(&mask);
                    result.is_worker_on_cpu = CPU_ISSET(result.cpu, &mask);
                }
            }
            if (::write(fds[1], "x", 1) == 1) {
                Dones done;
                result.is_read_done = reap(ring, 1, done);
            }
        }
    }
    result.unpinned = unregister_iowq_aff(ring);
    ::close(fds[0]);
    ::close(fds[1]);
}

}  // namespace

TEST_CASE(
    "uring register: iowq affinity pins the kernel workers of a new task to "
    "one CPU") {
    AffinityResult result;
    {
        dstd::Thread thread([&result] { run_affinity_case(result); });
        thread.join();
    }
    if (!result.is_ring_open) {
        MESSAGE("skipped: io_uring_setup refused the ring");
        return;
    }
    if (skip_unsupported(result.pinned, 5, 14, "register_iowq_aff")) return;
    REQUIRE(result.pinned == 0);
    REQUIRE(result.cpu >= 0);
    REQUIRE(result.is_worker_found);
    CHECK(result.worker_cpus == 1);
    CHECK(result.is_worker_on_cpu);
    CHECK(result.is_read_done);
    CHECK(result.unpinned == 0);
}

TEST_CASE(
    "uring register: a registered ring index serves enter and register calls") {
    Ring first;
    Ring second;
    OPEN_RING_OR_SKIP(first, 8, 0);
    OPEN_RING_OR_SKIP(second, 8, 0);

    unsigned first_index = 0;
    unsigned second_index = 0;
    REGISTER_OR_SKIP(registered, register_ring_fd(first, first_index), 5, 18,
                     "register_ring_fd");
    CHECK(registered == 0);
    REQUIRE(register_ring_fd(second, second_index) == 0);
    CHECK(first_index != second_index);

    const KernelTimespec short_wait = {0, 50000000};
    prep_timeout(next_sqe(first), &short_wait);
    REQUIRE(first.enter(false) == 1);
    const long waited =
        ::syscall(__NR_io_uring_enter, static_cast<int>(first_index), 0U, 1U,
                  IORING_ENTER_GETEVENTS | IORING_ENTER_REGISTERED_RING,
                  nullptr, static_cast<std::size_t>(0));
    CHECK(waited >= 0);
    Dones timed;
    REQUIRE(reap(first, 1, timed));
    CHECK(timed[0].res == -ETIME);

    if ((first.features() & IORING_FEAT_REG_REG_RING) == 0) {
        MESSAGE(
            "skipped the register-through-index step: a kernel before 6.3 "
            "has no IORING_FEAT_REG_REG_RING");
    } else {
        // The index names the second ring, so the personality lands there.
        use_registered_ring_once(second_index);
        const int id = register_personality(first);
        REQUIRE(id > 0);
        struct io_uring_sqe* sqe = next_sqe(second);
        prep_nop(sqe);
        set_personality(sqe, static_cast<unsigned>(id));
        Dones on_second;
        REQUIRE(reap(second, 1, on_second));
        CHECK(on_second[0].res == 0);
        sqe = next_sqe(first);
        prep_nop(sqe);
        set_personality(sqe, static_cast<unsigned>(id));
        Dones on_first;
        REQUIRE(reap(first, 1, on_first));
        CHECK(on_first[0].res == -EINVAL);
        CHECK(unregister_personality(second, id) == 0);
    }

    CHECK(unregister_ring_fd(first, first_index) == 0);
    CHECK(unregister_ring_fd(second, second_index) == 0);
}

TEST_CASE("uring register: sync_cancel ends a pending read") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    Pipe pipe;

    char buf[8];
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_read(sqe, pipe.read_end(), buf, sizeof(buf), 0);
    sqe->user_data = 5;
    REQUIRE(ring.enter(false) == 1);

    SyncCancelReg reg;
    reg.addr = 5;
    reg.fd = -1;
    reg.timeout = {2, 0};
    REGISTER_OR_SKIP(cancelled, sync_cancel(ring, reg), 6, 0, "sync_cancel");
    CHECK(cancelled == 0);
    Dones all;
    REQUIRE(reap(ring, 1, all));
    CHECK(all[0].user_data == 5);
    CHECK(all[0].res == -ECANCELED);
}

TEST_CASE("uring register: a registered clock times an absolute wait") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);

    ClockRegister bad;
    bad.clockid = CLOCK_REALTIME;
    const int refused = register_clock(ring, bad);
    if (skip_unsupported(refused, 6, 12, "register_clock")) return;
    CHECK(refused == -EINVAL);

    ClockRegister boot;
    boot.clockid = CLOCK_BOOTTIME;
    REQUIRE(register_clock(ring, boot) == 0);

    struct timespec now;
    REQUIRE(::clock_gettime(CLOCK_BOOTTIME, &now) == 0);
    KernelTimespec target = {static_cast<std::int64_t>(now.tv_sec),
                             static_cast<long long>(now.tv_nsec) + 100000000};
    if (target.tv_nsec >= 1000000000) {
        target.tv_sec += 1;
        target.tv_nsec -= 1000000000;
    }
    GeteventsArg arg;
    std::memset(&arg, 0, sizeof(arg));
    arg.sigmask_sz = _NSIG / 8;
    arg.ts = reinterpret_cast<std::uint64_t>(&target);
    const auto start = std::chrono::steady_clock::now();
    const long ret = ::syscall(
        __NR_io_uring_enter, ring.fd(), 0U, 1U,
        IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG | IORING_ENTER_ABS_TIMER,
        &arg, sizeof(arg));
    const int err = errno;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    CHECK(ret == -1);
    CHECK(err == ETIME);
    CHECK(elapsed.count() >= 80);
    CHECK(elapsed.count() < 3000);
}

TEST_CASE(
    "uring register: clone_buffers lets another ring use the registered "
    "buffers") {
    Ring source;
    Ring receiver;
    OPEN_RING_OR_SKIP(source, 8, 0);
    OPEN_RING_OR_SKIP(receiver, 8, 0);
    SKIP_UNLESS_OP(receiver, OP_READ_FIXED);
    TempFile file;
    file.write_pattern(256, 15);

    char in[256];
    std::memset(in, 0, sizeof(in));
    struct iovec buffer = {in, sizeof(in)};
    REQUIRE(register_buffers(source, &buffer, 1) == 0);
    REGISTER_OR_SKIP(cloned, clone_buffers(receiver, source.fd(), 0, 0, 0, 0),
                     6, 12, "clone_buffers");
    CHECK(cloned == 0);

    prep_read_fixed(next_sqe(receiver), file.fd(), in, sizeof(in), 0, 0);
    Dones read_done;
    REQUIRE(reap(receiver, 1, read_done));
    CHECK(read_done[0].res == static_cast<std::int32_t>(sizeof(in)));
    char want[256];
    fill_pattern(want, sizeof(want), 15);
    CHECK(std::memcmp(in, want, sizeof(in)) == 0);
}

TEST_CASE("uring register: resize_rings gives the rings the new sizes") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);

    struct io_uring_params params;
    std::memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_CQSIZE;
    params.sq_entries = 16;
    params.cq_entries = 64;
    REGISTER_OR_SKIP(resized, resize_rings(ring, params), 6, 13,
                     "resize_rings");
    CHECK(resized == 0);
    CHECK(params.sq_entries == 16);
    CHECK(params.cq_entries == 64);

    const std::size_t length =
        params.cq_off.cqes + static_cast<std::size_t>(params.cq_entries) *
                                 sizeof(struct io_uring_cqe);
    void* const mapped =
        ::mmap(nullptr, length, PROT_READ, MAP_SHARED | MAP_POPULATE, ring.fd(),
               IORING_OFF_CQ_RING);
    void* const mapped_single =
        mapped == MAP_FAILED
            ? ::mmap(nullptr, length, PROT_READ, MAP_SHARED | MAP_POPULATE,
                     ring.fd(), IORING_OFF_SQ_RING)
            : mapped;
    REQUIRE(mapped_single != MAP_FAILED);
    std::uint32_t entries = 0;
    std::memcpy(
        &entries,
        static_cast<const char*>(mapped_single) + params.cq_off.ring_entries,
        sizeof(entries));
    CHECK(entries == 64);
    ::munmap(mapped_single, length);
}

TEST_CASE("uring register: a wait region times an enter by its slot") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);

    const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    void* const memory = ::mmap(nullptr, page, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(memory != MAP_FAILED);
    RegWait* const waits = static_cast<RegWait*>(memory);
    waits[0].ts = {0, 30000000};
    waits[0].flags = REG_WAIT_TS;

    RegionDesc desc;
    desc.user_addr = reinterpret_cast<std::uint64_t>(memory);
    desc.size = page;
    desc.flags = static_cast<std::uint32_t>(MEM_REGION_TYPE_USER);
    MemRegionReg reg;
    reg.region_uptr = reinterpret_cast<std::uint64_t>(&desc);
    reg.flags = MEM_REGION_REG_WAIT_ARG;
    const int registered = register_mem_region(ring, reg);
    if (skip_unsupported(registered, 6, 13, "register_mem_region")) {
        ::munmap(memory, page);
        return;
    }
    REQUIRE(registered == 0);

    const auto start = std::chrono::steady_clock::now();
    const long ret =
        ::syscall(__NR_io_uring_enter, ring.fd(), 0U, 1U,
                  IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG_REG, 0UL,
                  sizeof(RegWait));
    const int err = errno;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    CHECK(ret == -1);
    CHECK(err == ETIME);
    CHECK(elapsed.count() >= 20);
    CHECK(elapsed.count() < 3000);
    ring.destroy();
    ::munmap(memory, page);
}

TEST_CASE("uring register: query counts the opcodes the probe sees") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    Probe probe;
    if (!ring.probe(probe)) {
        MESSAGE("skipped: a kernel before 5.6 has no probe");
        return;
    }

    QueryOpcode opcodes;
    QueryHdr hdr;
    hdr.query_data = reinterpret_cast<std::uint64_t>(&opcodes);
    hdr.query_op = QUERY_OPCODES;
    hdr.size = sizeof(opcodes);
    REGISTER_OR_SKIP(queried, register_query(hdr), 6, 18, "register_query");
    CHECK(queried == 0);
    CHECK(hdr.result == 0);
    CHECK(opcodes.nr_request_opcodes ==
          static_cast<unsigned>(probe.last_op) + 1);
    CHECK(opcodes.nr_register_opcodes > REG_QUERY);
}

TEST_CASE(
    "uring register: napi stores the busy poll setting and unregister returns "
    "it") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);

    Napi set;
    set.busy_poll_to = 50;
    set.prefer_busy_poll = 1;
    const int registered = register_napi(ring, set);
    if (skip_unsupported(registered, 6, 9, "register_napi")) return;
    if (is_environment_error(registered)) {
        MESSAGE("skipped: the kernel refused napi busy poll (error "
                << registered << ")");
        return;
    }
    REQUIRE(registered == 0);

    Napi old;
    REQUIRE(unregister_napi(ring, old) == 0);
    CHECK(old.busy_poll_to == 50);
    CHECK(old.prefer_busy_poll == 1);
}

TEST_CASE("uring register: napi static tracking takes and drops an id") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);

    Napi set;
    set.busy_poll_to = 50;
    set.prefer_busy_poll = 1;
    set.opcode = NAPI_OP_REGISTER;
    set.op_param = NAPI_TRACKING_STATIC;
    const int registered = register_napi(ring, set);
    if (skip_unsupported(registered, 6, 13, "napi static tracking")) return;
    if (is_environment_error(registered)) {
        MESSAGE("skipped: the kernel refused napi busy poll (error "
                << registered << ")");
        return;
    }
    REQUIRE(registered == 0);

    constexpr std::uint32_t NAPI_ID = 0x100000;
    Napi add;
    add.opcode = NAPI_OP_STATIC_ADD_ID;
    add.op_param = NAPI_ID;
    CHECK(register_napi(ring, add) == 0);
    Napi del;
    del.opcode = NAPI_OP_STATIC_DEL_ID;
    del.op_param = NAPI_ID;
    CHECK(register_napi(ring, del) == 0);

    Napi old;
    REQUIRE(unregister_napi(ring, old) == 0);
    CHECK(old.busy_poll_to == 50);
    CHECK(old.op_param == NAPI_TRACKING_STATIC);
}

TEST_CASE("uring register: a kernel allocated buffer ring serves a recv") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);
    SocketPair sockets;

    constexpr unsigned GROUP = 9;
    BufRing buf_ring;
    const int setup = buf_ring.setup(ring, 4, GROUP, PBUF_RING_MMAP);
    if (skip_unsupported(setup, 6, 4, "kernel allocated buffer rings")) return;
    if (skip_refused_buf_ring(setup)) return;
    REQUIRE(setup == 0);

    char pool[4][64];
    std::memset(pool, 0, sizeof(pool));
    for (unsigned i = 0; i < 4; ++i) buf_ring.add(pool[i], 64, i, i);
    buf_ring.advance(4);

    REQUIRE(::write(sockets.second(), "mapped", 6) == 6);
    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_recv(sqe, sockets.first(), nullptr, 64, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    set_buf_group(sqe, GROUP);
    sqe->user_data = 2;
    Dones received;
    REQUIRE(reap(ring, 1, received));
    REQUIRE(received[0].res == 6);
    REQUIRE((received[0].flags & IORING_CQE_F_BUFFER) != 0);
    const unsigned bid = done_buffer_id(received[0]);
    REQUIRE(bid < 4);
    CHECK(std::memcmp(pool[bid], "mapped", 6) == 0);
}

TEST_CASE("uring register: a kernel allocated buffer ring needs the ring fd") {
    Ring ring;
    RingOptions options;
    options.flags = IORING_SETUP_NO_MMAP | IORING_SETUP_REGISTERED_FD_ONLY;
    if (!ring.init(8, options)) {
        if (kernel_at_least(6, 5)) FAIL("a ring known by index was refused");
        MESSAGE("skipped: a kernel before 6.6 lacks REGISTERED_FD_ONLY");
        return;
    }
    REQUIRE(ring.is_fd_registered());
    BufRing buf_ring;
    CHECK(buf_ring.setup(ring, 4, 1, PBUF_RING_MMAP) == -EINVAL);
}

TEST_CASE("uring register: a zero copy receive queue registers and flushes") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8,
                      IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
                          IORING_SETUP_CQE32);
    const unsigned ifindex = ::if_nametoindex("lo");
    if (ifindex == 0) {
        MESSAGE("skipped: no loopback interface for the zcrx queue");
        return;
    }

    constexpr std::size_t AREA_SIZE = 1U << 20;
    constexpr std::size_t REGION_SIZE = 1U << 14;
    void* const area_memory = ::mmap(nullptr, AREA_SIZE, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* const region_memory =
        ::mmap(nullptr, REGION_SIZE, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(area_memory != MAP_FAILED);
    REQUIRE(region_memory != MAP_FAILED);

    ZcrxAreaReg area;
    area.addr = reinterpret_cast<std::uint64_t>(area_memory);
    area.len = AREA_SIZE;
    RegionDesc region;
    region.user_addr = reinterpret_cast<std::uint64_t>(region_memory);
    region.size = REGION_SIZE;
    region.flags = static_cast<std::uint32_t>(MEM_REGION_TYPE_USER);
    ZcrxIfqReg reg;
    reg.if_idx = ifindex;
    reg.if_rxq = 0;
    reg.rq_entries = 64;
    reg.area_ptr = reinterpret_cast<std::uint64_t>(&area);
    reg.region_ptr = reinterpret_cast<std::uint64_t>(&region);
    const int registered = register_zcrx_ifq(ring, reg);
    if (skip_unsupported(registered, 6, 15, "register_zcrx_ifq") ||
        is_environment_error(registered)) {
        if (registered < 0 && is_environment_error(registered)) {
            MESSAGE(
                "skipped: no zero copy receive queue on the loopback "
                "interface (error "
                << registered << ")");
        }
        ::munmap(area_memory, AREA_SIZE);
        ::munmap(region_memory, REGION_SIZE);
        return;
    }
    REQUIRE(registered == 0);

    ZcrxCtrl ctrl;
    ctrl.zcrx_id = reg.zcrx_id;
    ctrl.op = ZCRX_CTRL_FLUSH_RQ;
    const int flushed = zcrx_ctrl(ring, ctrl);
    if (!skip_unsupported(flushed, 6, 19, "zcrx_ctrl")) {
        CHECK(flushed == 0);
    }
    ring.destroy();
    ::munmap(area_memory, AREA_SIZE);
    ::munmap(region_memory, REGION_SIZE);
}

TEST_CASE("uring register: a BPF filter that denies NOP refuses it") {
    Ring ring;
    OPEN_RING_OR_SKIP(ring, 8, 0);

    struct sock_filter program[1] = {BPF_STMT(BPF_RET | BPF_K, 0)};
    Bpf bpf;
    bpf.cmd_type = URING_BPF_CMD_FILTER;
    bpf.filter.opcode = 0;
    bpf.filter.filter_len = 1;
    bpf.filter.filter_ptr = reinterpret_cast<std::uint64_t>(program);
    const int registered = register_bpf_filter(ring, bpf);
    if (skip_unsupported(registered, 7, 0, "register_bpf_filter")) return;
    if (is_environment_error(registered)) {
        MESSAGE("skipped: the kernel refused the BPF filter (error "
                << registered << ")");
        return;
    }
    REQUIRE(registered == 0);

    struct io_uring_sqe* sqe = next_sqe(ring);
    prep_nop(sqe);
    sqe->user_data = 1;
    Dones all;
    REQUIRE(reap(ring, 1, all));
    INFO("NOP result " << all[0].res);
    CHECK(all[0].res < 0);
}

#endif  // DFTRACER_UTILS_HAVE_IO_URING

TEST_CASE("uring register: a destroyed ring gives its registered slot back") {
    // A thread has 16 slots, so this count runs out if a slot stays taken.
    for (int round = 0; round < 40; ++round) {
        Ring ring;
        OPEN_RING_OR_SKIP(ring, 8, 0);
        unsigned index = 0;
        const int ret = register_ring_fd(ring, index);
        if (skip_unsupported(ret, 5, 18, "register_ring_fd")) return;
        REQUIRE(ret == 0);
        ring.enter_through_index(index);
    }
}
