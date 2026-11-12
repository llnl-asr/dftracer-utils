// SPDX-License-Identifier: MIT
#include <dftracer/utils/runtime/io/io_sendfile.h>
#include <dftracer/utils/runtime/io/thread_pool_file_ops.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#endif

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>

namespace dftracer::utils::runtime::io {

namespace {
// A call that may wait indefinitely, on a peer or on a wake, is not tracked.
bool is_file_op(IoOpKind kind) {
    switch (kind) {
        case IoOpKind::Accept:
        case IoOpKind::Recv:
        case IoOpKind::Send:
        case IoOpKind::Connect:
        case IoOpKind::Sendmsg:
        case IoOpKind::Recvmsg:
        case IoOpKind::Splice:
        case IoOpKind::Tee:
        case IoOpKind::Waitid:
        case IoOpKind::FutexWait:
        case IoOpKind::FutexWaitv:
        case IoOpKind::EpollWait:
        case IoOpKind::PollAdd:
            return false;
        default:
            return true;
    }
}

#ifdef __linux__
#ifdef SYS_openat2
constexpr long SYSCALL_OPENAT2 = SYS_openat2;
#else
constexpr long SYSCALL_OPENAT2 = 437;
#endif
#ifdef SYS_futex_waitv
constexpr long SYSCALL_FUTEX_WAITV = SYS_futex_waitv;
#else
constexpr long SYSCALL_FUTEX_WAITV = 449;
#endif

constexpr long FUTEX_WAIT_BITSET_OP = 9;
constexpr long FUTEX_WAKE_BITSET_OP = 10;
constexpr long FUTEX_PRIVATE_OP = 128;
constexpr unsigned FUTEX_SIZE_MASK = 0x03U;

long futex_operation(long operation, int futex_flags) {
    return (static_cast<unsigned>(futex_flags) & IO_FUTEX_PRIVATE) != 0
               ? operation | FUTEX_PRIVATE_OP
               : operation;
}
#endif

// Operations whose syscall can block on a peer for ever, so cancel_pending()
// has to be able to interrupt their wait. File writes are left out: they do
// not wait for a peer and the extra poll would slow every sequential write.
bool waits_for_peer(const IoOp& op, short& events) {
    switch (op.kind) {
        case IoOpKind::Read:
        case IoOpKind::Readv:
        case IoOpKind::Recv:
        case IoOpKind::Recvmsg:
        case IoOpKind::Accept:
        case IoOpKind::Splice:
        case IoOpKind::Tee:
        case IoOpKind::EpollWait:
            events = POLLIN;
            return true;
        case IoOpKind::Send:
        case IoOpKind::Sendmsg:
            events = POLLOUT;
            return true;
        case IoOpKind::PollAdd:
            events = static_cast<short>(op.flags);
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
bool wait_ready(int fd, short events, int out_fd, const IoThreadPool& pool) {
    // poll() ignores a negative descriptor and would wait out every slice.
    if (fd < 0) return true;
    constexpr int SLICE_MS = 20;
    struct pollfd pfds[2] = {{fd, events, 0}, {out_fd, POLLOUT, 0}};
    while (!pool.cancelling()) {
        const int n = ::poll(pfds, 2, SLICE_MS);
        if (n < 0) return true;
        for (struct pollfd& pfd : pfds) {
            if (pfd.revents != 0) pfd.fd = -1;
        }
        if (pfds[0].fd < 0 && pfds[1].fd < 0) return true;
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

// A ring send never raises SIGPIPE, so a pool send must not either.
#ifdef MSG_NOSIGNAL
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0;
#endif

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
            result =
                ::send(op.handle, op.buffer, op.length, op.flags | SEND_FLAGS);
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
        case IoOpKind::Statx:
#ifdef __linux__
            result = ::syscall(SYS_statx, op.handle, op.path, op.flags,
                               op.statx_mask, op.statx_out);
#else
            errno = ENOSYS;
            result = -1;
#endif
            break;
        case IoOpKind::Unlink:
            result = ::unlinkat(AT_FDCWD, op.path, op.flags);
            break;
        case IoOpKind::Rename:
            result = ::rename(op.path, op.path2);
            break;
        case IoOpKind::Mkdir:
            result = ::mkdir(op.path, op.mode);
            break;
        case IoOpKind::Fallocate:
#ifdef __linux__
            result = ::fallocate(op.handle, op.flags, op.offset, op.length);
#else
            errno = ENOSYS;
            result = -1;
#endif
            break;
        case IoOpKind::Fadvise:
#ifdef __linux__
            // posix_fadvise gives the error number and leaves errno alone.
            if (const int error = ::posix_fadvise(op.handle, op.offset,
                                                  op.length, op.flags)) {
                errno = error;
                result = -1;
            }
#endif
            break;
        case IoOpKind::SyncFileRange:
#ifdef __linux__
            result = ::sync_file_range(op.handle, op.offset, op.length,
                                       static_cast<unsigned>(op.flags));
#else
            errno = ENOSYS;
            result = -1;
#endif
            break;
        case IoOpKind::Splice: {
#ifdef __linux__
            loff_t in_offset = op.offset;
            loff_t out_offset = op.out_offset;
            result = ::splice(op.handle, op.offset < 0 ? nullptr : &in_offset,
                              op.out_handle,
                              op.out_offset < 0 ? nullptr : &out_offset,
                              op.length, static_cast<unsigned>(op.flags));
#else
            errno = ENOSYS;
            result = -1;
#endif
            break;
        }
        case IoOpKind::Linkat:
            result =
                ::linkat(op.handle, op.path, op.out_handle, op.path2, op.flags);
            break;
        case IoOpKind::Symlinkat:
            result = ::symlinkat(op.path, op.handle, op.path2);
            break;
        case IoOpKind::Socket:
            result = ::socket(op.handle, static_cast<int>(op.offset), op.flags);
            break;
        case IoOpKind::Connect:
            result = ::connect(op.handle, op.addr,
                               static_cast<socklen_t>(op.length));
            break;
        case IoOpKind::Bind:
            result =
                ::bind(op.handle, op.addr, static_cast<socklen_t>(op.length));
            break;
        case IoOpKind::Listen:
            result = ::listen(op.handle, op.flags);
            break;
        case IoOpKind::Shutdown:
            result = ::shutdown(op.handle, op.flags);
            break;
        case IoOpKind::Sendmsg:
            result = ::sendmsg(op.handle, op.msg, op.flags | SEND_FLAGS);
            break;
        case IoOpKind::Recvmsg:
            result = ::recvmsg(op.handle, op.msg, op.flags);
            break;
        case IoOpKind::Madvise:
            result = ::madvise(op.buffer, op.length, op.flags);
            break;
        case IoOpKind::Waitid:
            do {
                result =
                    ::waitid(static_cast<idtype_t>(op.idtype),
                             static_cast<id_t>(op.handle), op.info, op.flags);
            } while (result < 0 && errno == EINTR);
            break;
        case IoOpKind::PollAdd: {
            struct pollfd pfd{op.handle, static_cast<short>(op.flags), 0};
            if (op.handle < 0) {
                errno = EBADF;
                result = -1;
                break;
            }
            do {
                result = ::poll(&pfd, 1, -1);
            } while (result < 0 && errno == EINTR);
            if (result > 0 && (pfd.revents & POLLNVAL) != 0) {
                errno = EBADF;
                result = -1;
            } else if (result > 0) {
                result = pfd.revents;
            }
            break;
        }
#ifdef __linux__
        case IoOpKind::Openat2:
            result = ::syscall(SYSCALL_OPENAT2, op.handle, op.path, op.how,
                               sizeof(OpenHow));
            break;
        case IoOpKind::Getxattr:
            result = ::getxattr(op.path, op.xattr->name, op.xattr->value,
                                op.xattr->size);
            break;
        case IoOpKind::Setxattr:
            result = ::setxattr(op.path, op.xattr->name, op.xattr->value,
                                op.xattr->size, op.xattr->flags);
            break;
        case IoOpKind::Fgetxattr:
            result = ::fgetxattr(op.handle, op.xattr->name, op.xattr->value,
                                 op.xattr->size);
            break;
        case IoOpKind::Fsetxattr:
            result = ::fsetxattr(op.handle, op.xattr->name, op.xattr->value,
                                 op.xattr->size, op.xattr->flags);
            break;
        case IoOpKind::Tee:
            result = ::tee(op.handle, op.out_handle, op.length,
                           static_cast<unsigned>(op.flags));
            break;
        case IoOpKind::Pipe:
            result = ::pipe2(op.pipe_fds, op.flags);
            break;
        case IoOpKind::FutexWait:
            if ((static_cast<unsigned>(op.flags) & FUTEX_SIZE_MASK) !=
                IO_FUTEX_SIZE_U32) {
                errno = EINVAL;
                result = -1;
                break;
            }
            do {
                result = ::syscall(
                    SYS_futex, op.futex,
                    futex_operation(FUTEX_WAIT_BITSET_OP, op.flags),
                    static_cast<std::uint32_t>(op.offset), nullptr, nullptr,
                    static_cast<std::uint32_t>(op.futex_mask));
            } while (result < 0 && errno == EINTR);
            break;
        case IoOpKind::FutexWake:
            if ((static_cast<unsigned>(op.flags) & FUTEX_SIZE_MASK) !=
                IO_FUTEX_SIZE_U32) {
                errno = EINVAL;
                result = -1;
                break;
            }
            result =
                ::syscall(SYS_futex, op.futex,
                          futex_operation(FUTEX_WAKE_BITSET_OP, op.flags),
                          static_cast<std::uint32_t>(op.offset), nullptr,
                          nullptr, static_cast<std::uint32_t>(op.futex_mask));
            break;
        case IoOpKind::FutexWaitv:
            do {
                result =
                    ::syscall(SYSCALL_FUTEX_WAITV, op.futex_waitv,
                              static_cast<unsigned>(op.length), 0U, nullptr, 0);
            } while (result < 0 && errno == EINTR);
            break;
        case IoOpKind::EpollCtl:
            result = ::epoll_ctl(op.handle, op.flags,
                                 static_cast<int>(op.offset), op.epoll_events);
            break;
        case IoOpKind::EpollWait:
            do {
                result = ::epoll_wait(op.handle, op.epoll_events, op.flags, -1);
            } while (result < 0 && errno == EINTR);
            break;
#else
        case IoOpKind::Openat2:
        case IoOpKind::Getxattr:
        case IoOpKind::Setxattr:
        case IoOpKind::Fgetxattr:
        case IoOpKind::Fsetxattr:
        case IoOpKind::Tee:
        case IoOpKind::Pipe:
        case IoOpKind::FutexWait:
        case IoOpKind::FutexWake:
        case IoOpKind::FutexWaitv:
        case IoOpKind::EpollCtl:
        case IoOpKind::EpollWait:
            errno = ENOSYS;
            result = -1;
            break;
#endif
    }
    return result < 0 ? -errno : result;
}

void ThreadPoolFileOps::execute(IoOp& op) noexcept {
    ssize_t result = 0;
    short events = 0;
    const bool may_wait =
        waits_for_peer(op, events) &&
        !((op.kind == IoOpKind::Read || op.kind == IoOpKind::Readv) &&
          is_regular_file(op.handle));
    const bool has_two_ends =
        op.kind == IoOpKind::Splice || op.kind == IoOpKind::Tee;
    if (may_wait && !wait_ready(op.handle, events,
                                has_two_ends ? op.out_handle : -1, pool_)) {
        result = -ECANCELED;
    } else {
        result = run_blocking(op);
        if (op.kind == IoOpKind::Close) {
            forget_fd_kind(op.handle);
        } else if ((op.kind == IoOpKind::Open || op.kind == IoOpKind::Openat2 ||
                    op.kind == IoOpKind::Socket) &&
                   result >= 0) {
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
