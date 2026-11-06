// SPDX-License-Identifier: MIT
#include <dftracer/utils/runtime/io/io_sendfile.h>
#include <dftracer/utils/runtime/io/thread_pool_file_ops.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>

namespace dftracer::utils::runtime::io {

namespace {
// A socket call may wait on a peer indefinitely, so it is not tracked.
bool is_file_op(IoOpKind kind) {
    return kind != IoOpKind::Accept && kind != IoOpKind::Recv &&
           kind != IoOpKind::Send;
}

// Operations whose syscall can block on a peer for ever, so cancel_pending()
// has to be able to interrupt their wait. File writes are left out: they do
// not wait for a peer and the extra poll would slow every sequential write.
bool waits_for_peer(IoOpKind kind, short& events) {
    switch (kind) {
        case IoOpKind::Read:
        case IoOpKind::Readv:
        case IoOpKind::Recv:
        case IoOpKind::Accept:
            events = POLLIN;
            return true;
        case IoOpKind::Send:
            events = POLLOUT;
            return true;
        default:
            return false;
    }
}

// A regular file never waits for a peer, so a read of one skips the cancellable
// wait. The type is looked up once per descriptor. The entry is dropped when
// the descriptor is closed or opened through this backend. One closed and
// reused behind its back keeps the old answer.
constexpr int FD_KIND_SLOTS = 4096;
constexpr std::uint8_t FD_UNKNOWN = 0;
constexpr std::uint8_t FD_REGULAR = 1;
constexpr std::uint8_t FD_OTHER = 2;
std::atomic<std::uint8_t> g_fd_kind[FD_KIND_SLOTS];

bool is_regular_file(int fd) {
    if (fd < 0 || fd >= FD_KIND_SLOTS) return false;
    std::uint8_t kind = g_fd_kind[fd].load(std::memory_order_relaxed);
    if (kind == FD_UNKNOWN) {
        struct stat st;
        kind = (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) ? FD_REGULAR
                                                              : FD_OTHER;
        g_fd_kind[fd].store(kind, std::memory_order_relaxed);
    }
    return kind == FD_REGULAR;
}

void forget_fd_kind(int fd) {
    if (fd >= 0 && fd < FD_KIND_SLOTS) {
        g_fd_kind[fd].store(FD_UNKNOWN, std::memory_order_relaxed);
    }
}

// False when the pool was cancelled before the descriptor became ready. An
// error or hang-up counts as ready: the syscall then reports it.
bool wait_ready(int fd, short events, const IoThreadPool& pool) {
    // poll() ignores a negative descriptor and would wait out every slice.
    if (fd < 0) return true;
    constexpr int SLICE_MS = 20;
    struct pollfd pfd{fd, events, 0};
    while (!pool.cancelling()) {
        const int n = ::poll(&pfd, 1, SLICE_MS);
        if (n != 0) return true;
    }
    return false;
}
}  // namespace

ThreadPoolFileOps::ThreadPoolFileOps(std::size_t pool_size,
                                     unsigned batch_threshold,
                                     IoPoolOptions pool_options)
    : pool_(pool_size, batch_threshold, pool_options) {}

void ThreadPoolFileOps::submit(IoOp& op) {
    if (is_file_op(op.kind)) pool_.begin_tracked();
    pool_.submit([this, &op] { execute(op); });
}

ssize_t run_blocking(const IoOp& op) noexcept {
    ssize_t result = 0;
    switch (op.kind) {
        case IoOpKind::Read:
            result = ::read(op.handle, op.buffer, op.length);
            break;
        case IoOpKind::Write:
            result = ::write(op.handle, op.buffer, op.length);
            break;
        case IoOpKind::Pread:
            result = ::pread(op.handle, op.buffer, op.length, op.offset);
            break;
        case IoOpKind::Pwrite:
            result = ::pwrite(op.handle, op.buffer, op.length, op.offset);
            break;
        case IoOpKind::Open:
            result = ::open(op.path, op.flags, op.mode);
            break;
        case IoOpKind::Close:
            result = ::close(op.handle);
            break;
        case IoOpKind::Fsync:
            result = ::fsync(op.handle);
            break;
        case IoOpKind::Ftruncate:
            result = ::ftruncate(op.handle, op.offset);
            break;
        case IoOpKind::Fstat:
            result = ::fstat(op.handle, op.stat_out);
            break;
        case IoOpKind::Call:
            result = op.call(op.call_context);
            break;
        case IoOpKind::Accept:
#ifdef __linux__
            result = ::accept4(op.handle, op.addr, op.addrlen,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
            result = ::accept(op.handle, op.addr, op.addrlen);
            if (result >= 0) {
                int fd = static_cast<int>(result);
                int fl = ::fcntl(fd, F_GETFL, 0);
                ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
                ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
#endif
            break;
        case IoOpKind::Recv:
            result = ::recv(op.handle, op.buffer, op.length, op.flags);
            break;
        case IoOpKind::Send:
            result = ::send(op.handle, op.buffer, op.length, op.flags);
            break;
        case IoOpKind::Readv:
            result = ::readv(op.handle, op.iov, op.iov_count);
            break;
        case IoOpKind::Writev:
            result = ::writev(op.handle, op.iov, op.iov_count);
            break;
        case IoOpKind::Preadv:
            result = ::preadv(op.handle, op.iov, op.iov_count, op.offset);
            break;
        case IoOpKind::Pwritev:
            result = ::pwritev(op.handle, op.iov, op.iov_count, op.offset);
            break;
        case IoOpKind::Lseek:
            result = ::lseek(op.handle, op.offset, op.whence);
            break;
        case IoOpKind::Sendfile:
            result = platform_sendfile(op.out_handle, op.handle, op.offset,
                                       op.length);
            break;
    }
    return result < 0 ? -errno : result;
}

void ThreadPoolFileOps::execute(IoOp& op) noexcept {
    ssize_t result = 0;
    short events = 0;
    const bool may_wait =
        waits_for_peer(op.kind, events) &&
        !((op.kind == IoOpKind::Read || op.kind == IoOpKind::Readv) &&
          is_regular_file(op.handle));
    if (may_wait && !wait_ready(op.handle, events, pool_)) {
        result = -ECANCELED;
    } else {
        result = run_blocking(op);
        if (op.kind == IoOpKind::Close) {
            forget_fd_kind(op.handle);
        } else if (op.kind == IoOpKind::Open && result >= 0) {
            forget_fd_kind(static_cast<int>(result));
        }
    }

    const bool tracked = is_file_op(op.kind);
    op.result = result;
    op.complete(op);
    if (tracked) pool_.end_tracked();
}

std::size_t ThreadPoolFileOps::poll(int /*timeout_ms*/) {
    return pool_.flush();
}

int ThreadPoolFileOps::flush() { return static_cast<int>(pool_.flush()); }

}  // namespace dftracer::utils::runtime::io
