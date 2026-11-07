// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/dstd/os.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/io/ops.h>
#include <dftracer/utils/runtime/io/thread_pool_file_ops.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>

namespace dftracer::utils::runtime::io {

namespace {

// Set on the threads an executor starts. The owner is read with the backend
// so a suppressed executor falls back without a second lookup.
struct BackendSlot {
    const coro::Executor* owner = nullptr;
    IoBackend* backend = nullptr;
};

thread_local BackendSlot t_backend;

// Max iovec entries writev(2) accepts. glibc only exposes IOV_MAX under XOPEN
// feature macros and spells it UIO_MAXIOV; sysconf is the portable query.
// io_uring's IORING_OP_WRITEV is bounded by the same limit.
int iov_max_entries() noexcept {
    static const int value = [] {
        long n = ::sysconf(_SC_IOV_MAX);
        if (n > 0) return static_cast<int>(n);
#if defined(IOV_MAX)
        return static_cast<int>(IOV_MAX);
#elif defined(UIO_MAXIOV)
        return static_cast<int>(UIO_MAXIOV);
#else
        return 1024;
#endif
    }();
    return value;
}

// With a backend the op runs when awaited. Without one it runs here.
IoAwaitable dispatch(IoAwaitable op) noexcept {
    if (IoBackend* backend = current_backend()) {
        op.backend = backend;
        op.resume_on = coro::Executor::current();
    } else {
        op.result = run_blocking(op);
    }
    return op;
}

IoAwaitable make_op(IoOpKind kind, dstd::NativeHandle fd) noexcept {
    IoAwaitable op;
    op.kind = kind;
    op.handle = fd;
    return op;
}

IoAwaitable make_buffer_op(IoOpKind kind, dstd::NativeHandle fd, void* buf,
                           std::size_t len, std::uint64_t offset = 0,
                           int flags = 0) noexcept {
    IoAwaitable op = make_op(kind, fd);
    op.buffer = buf;
    op.length = len;
    op.offset = static_cast<std::int64_t>(offset);
    op.flags = flags;
    return op;
}

IoAwaitable make_vector_op(IoOpKind kind, dstd::NativeHandle fd,
                           const struct iovec* iov, int iovcnt,
                           std::uint64_t offset = 0) noexcept {
    IoAwaitable op = make_op(kind, fd);
    op.iov = iov;
    op.iov_count = iovcnt;
    op.offset = static_cast<std::int64_t>(offset);
    return op;
}

}  // namespace

IoBackend* current_backend() noexcept {
    return coro::Executor::current() == t_backend.owner ? t_backend.backend
                                                        : nullptr;
}

void set_current_backend(const coro::Executor* owner,
                         IoBackend* backend) noexcept {
    t_backend.owner = owner;
    t_backend.backend = backend;
}

IoAwaitable read(dstd::NativeHandle fd, void* buf, std::size_t len) noexcept {
    return dispatch(make_buffer_op(IoOpKind::Read, fd, buf, len));
}

IoAwaitable write(dstd::NativeHandle fd, const void* buf,
                  std::size_t len) noexcept {
    return dispatch(
        make_buffer_op(IoOpKind::Write, fd, const_cast<void*>(buf), len));
}

IoAwaitable pread(dstd::NativeHandle fd, void* buf, std::size_t len,
                  std::uint64_t offset) noexcept {
    return dispatch(make_buffer_op(IoOpKind::Pread, fd, buf, len, offset));
}

IoAwaitable pwrite(dstd::NativeHandle fd, const void* buf, std::size_t len,
                   std::uint64_t offset) noexcept {
    return dispatch(make_buffer_op(IoOpKind::Pwrite, fd, const_cast<void*>(buf),
                                   len, offset));
}

IoAwaitable open(const char* path, int flags, mode_t mode) noexcept {
    IoAwaitable op = make_op(IoOpKind::Open, -1);
    op.path = path;
    op.flags = flags;
    op.mode = mode;
    return dispatch(op);
}

coro::CoroTask<dstd::Expected<dstd::File, dstd::Error>> open_file(
    dstd::PathView path, dstd::OpenOptions options) {
    auto flags = dstd::File::native_open_flags(options);
    if (!flags) co_return dstd::Unexpected(flags.error());
    const dstd::internal::PathCString c_path(path.str());
    ssize_t fd = -EINTR;
    while (fd == -EINTR) {
        fd = co_await open(c_path.c_str(), *flags,
                           static_cast<mode_t>(options.mode));
    }
    if (fd < 0) {
        co_return dstd::Unexpected(
            dstd::internal::errno_error(static_cast<int>(-fd), path.str()));
    }
    co_return dstd::File::from_native_handle(static_cast<int>(fd));
}

IoAwaitable close(dstd::NativeHandle fd) noexcept {
    return dispatch(make_op(IoOpKind::Close, fd));
}

IoAwaitable fsync(dstd::NativeHandle fd) noexcept {
    return dispatch(make_op(IoOpKind::Fsync, fd));
}

IoAwaitable ftruncate(dstd::NativeHandle fd, std::uint64_t length) noexcept {
    IoAwaitable op = make_op(IoOpKind::Ftruncate, fd);
    op.offset = static_cast<std::int64_t>(length);
    return dispatch(op);
}

IoAwaitable fstat(dstd::NativeHandle fd, struct stat* buf) noexcept {
    IoAwaitable op = make_op(IoOpKind::Fstat, fd);
    op.stat_out = buf;
    return dispatch(op);
}

IoAwaitable unlink(const char* path) noexcept {
    IoAwaitable op = make_op(IoOpKind::Unlink, -1);
    op.path = path;
    return dispatch(op);
}

IoAwaitable rmdir(const char* path) noexcept {
    IoAwaitable op = make_op(IoOpKind::Unlink, -1);
    op.path = path;
    op.flags = AT_REMOVEDIR;
    return dispatch(op);
}

IoAwaitable rename(const char* from, const char* to) noexcept {
    IoAwaitable op = make_op(IoOpKind::Rename, -1);
    op.path = from;
    op.path2 = to;
    return dispatch(op);
}

IoAwaitable mkdir(const char* path, mode_t mode) noexcept {
    IoAwaitable op = make_op(IoOpKind::Mkdir, -1);
    op.path = path;
    op.mode = mode;
    return dispatch(op);
}

IoAwaitable fadvise(dstd::NativeHandle fd, std::uint64_t offset,
                    std::uint64_t length, int advice) noexcept {
    IoAwaitable op = make_op(IoOpKind::Fadvise, fd);
    op.offset = static_cast<std::int64_t>(offset);
    op.length = static_cast<std::size_t>(length);
    op.flags = advice;
    return dispatch(op);
}

IoAwaitable linkat(dstd::NativeHandle from_dirfd, const char* from,
                   dstd::NativeHandle to_dirfd, const char* to,
                   int flags) noexcept {
    IoAwaitable op = make_op(IoOpKind::Linkat, from_dirfd);
    op.path = from;
    op.path2 = to;
    op.out_handle = to_dirfd;
    op.flags = flags;
    return dispatch(op);
}

IoAwaitable symlinkat(const char* target, dstd::NativeHandle dirfd,
                      const char* link) noexcept {
    IoAwaitable op = make_op(IoOpKind::Symlinkat, dirfd);
    op.path = target;
    op.path2 = link;
    return dispatch(op);
}

IoAwaitable socket(int domain, int type, int protocol) noexcept {
    IoAwaitable op = make_op(IoOpKind::Socket, domain);
    op.offset = type;
    op.flags = protocol;
    return dispatch(op);
}

IoAwaitable connect(dstd::NativeHandle fd, const struct sockaddr* addr,
                    socklen_t addrlen) noexcept {
    IoAwaitable op = make_op(IoOpKind::Connect, fd);
    op.addr = const_cast<struct sockaddr*>(addr);
    op.length = addrlen;
    return dispatch(op);
}

IoAwaitable bind(dstd::NativeHandle fd, const struct sockaddr* addr,
                 socklen_t addrlen) noexcept {
    IoAwaitable op = make_op(IoOpKind::Bind, fd);
    op.addr = const_cast<struct sockaddr*>(addr);
    op.length = addrlen;
    return dispatch(op);
}

IoAwaitable listen(dstd::NativeHandle fd, int backlog) noexcept {
    IoAwaitable op = make_op(IoOpKind::Listen, fd);
    op.flags = backlog;
    return dispatch(op);
}

IoAwaitable shutdown(dstd::NativeHandle fd, int how) noexcept {
    IoAwaitable op = make_op(IoOpKind::Shutdown, fd);
    op.flags = how;
    return dispatch(op);
}

IoAwaitable sendmsg(dstd::NativeHandle fd, const struct msghdr* msg,
                    int flags) noexcept {
    IoAwaitable op = make_op(IoOpKind::Sendmsg, fd);
    op.msg = const_cast<struct msghdr*>(msg);
    op.flags = flags;
    return dispatch(op);
}

IoAwaitable recvmsg(dstd::NativeHandle fd, struct msghdr* msg,
                    int flags) noexcept {
    IoAwaitable op = make_op(IoOpKind::Recvmsg, fd);
    op.msg = msg;
    op.flags = flags;
    return dispatch(op);
}

IoAwaitable madvise(void* addr, std::size_t length, int advice) noexcept {
    IoAwaitable op = make_op(IoOpKind::Madvise, -1);
    op.buffer = addr;
    op.length = length;
    op.flags = advice;
    return dispatch(op);
}

IoAwaitable waitid(int idtype, id_t id, siginfo_t* info, int options) noexcept {
    IoAwaitable op = make_op(IoOpKind::Waitid, static_cast<int>(id));
    op.info = info;
    op.idtype = idtype;
    op.flags = options;
    return dispatch(op);
}

IoAwaitable poll_add(dstd::NativeHandle fd, unsigned events) noexcept {
    IoAwaitable op = make_op(IoOpKind::PollAdd, fd);
    op.flags = static_cast<int>(events);
    return dispatch(op);
}

#ifdef __linux__
IoAwaitable statx(dstd::NativeHandle dirfd, const char* path, int flags,
                  std::uint32_t mask, Statx* out) noexcept {
    IoAwaitable op = make_op(IoOpKind::Statx, dirfd);
    op.path = path;
    op.flags = flags;
    op.statx_mask = mask;
    op.statx_out = out;
    return dispatch(op);
}

IoAwaitable fallocate(dstd::NativeHandle fd, int mode, std::uint64_t offset,
                      std::uint64_t length) noexcept {
    IoAwaitable op = make_op(IoOpKind::Fallocate, fd);
    op.offset = static_cast<std::int64_t>(offset);
    op.length = static_cast<std::size_t>(length);
    op.flags = mode;
    return dispatch(op);
}

IoAwaitable sync_file_range(dstd::NativeHandle fd, std::uint64_t offset,
                            std::uint64_t length, unsigned flags) noexcept {
    IoAwaitable op = make_op(IoOpKind::SyncFileRange, fd);
    op.offset = static_cast<std::int64_t>(offset);
    op.length = static_cast<std::size_t>(length);
    op.flags = static_cast<int>(flags);
    return dispatch(op);
}

IoAwaitable splice(dstd::NativeHandle in_fd, std::int64_t in_offset,
                   dstd::NativeHandle out_fd, std::int64_t out_offset,
                   std::size_t length, unsigned flags) noexcept {
    IoAwaitable op = make_op(IoOpKind::Splice, in_fd);
    op.offset = in_offset;
    op.out_offset = out_offset;
    op.out_handle = out_fd;
    op.length = length;
    op.flags = static_cast<int>(flags);
    return dispatch(op);
}

IoAwaitable openat2(dstd::NativeHandle dirfd, const char* path,
                    const OpenHow& how) noexcept {
    IoAwaitable op = make_op(IoOpKind::Openat2, dirfd);
    op.path = path;
    op.how = &how;
    return dispatch(op);
}

IoAwaitable getxattr(const char* path, const XattrArgs& args) noexcept {
    IoAwaitable op = make_op(IoOpKind::Getxattr, -1);
    op.path = path;
    op.xattr = &args;
    return dispatch(op);
}

IoAwaitable setxattr(const char* path, const XattrArgs& args) noexcept {
    IoAwaitable op = make_op(IoOpKind::Setxattr, -1);
    op.path = path;
    op.xattr = &args;
    return dispatch(op);
}

IoAwaitable fgetxattr(dstd::NativeHandle fd, const XattrArgs& args) noexcept {
    IoAwaitable op = make_op(IoOpKind::Fgetxattr, fd);
    op.xattr = &args;
    return dispatch(op);
}

IoAwaitable fsetxattr(dstd::NativeHandle fd, const XattrArgs& args) noexcept {
    IoAwaitable op = make_op(IoOpKind::Fsetxattr, fd);
    op.xattr = &args;
    return dispatch(op);
}

IoAwaitable tee(dstd::NativeHandle in_fd, dstd::NativeHandle out_fd,
                std::size_t length, unsigned flags) noexcept {
    IoAwaitable op = make_op(IoOpKind::Tee, in_fd);
    op.out_handle = out_fd;
    op.length = length;
    op.flags = static_cast<int>(flags);
    return dispatch(op);
}

IoAwaitable pipe(int* fds, int flags) noexcept {
    IoAwaitable op = make_op(IoOpKind::Pipe, -1);
    op.pipe_fds = fds;
    op.flags = flags;
    return dispatch(op);
}

IoAwaitable futex_wait(const std::uint32_t* uaddr, std::uint32_t expected,
                       std::uint64_t mask, unsigned futex_flags) noexcept {
    IoAwaitable op = make_op(IoOpKind::FutexWait, -1);
    op.futex = const_cast<std::uint32_t*>(uaddr);
    op.offset = expected;
    op.futex_mask = mask;
    op.flags = static_cast<int>(futex_flags);
    return dispatch(op);
}

IoAwaitable futex_wake(std::uint32_t* uaddr, std::uint32_t count,
                       std::uint64_t mask, unsigned futex_flags) noexcept {
    IoAwaitable op = make_op(IoOpKind::FutexWake, -1);
    op.futex = uaddr;
    op.offset = count;
    op.futex_mask = mask;
    op.flags = static_cast<int>(futex_flags);
    return dispatch(op);
}

IoAwaitable futex_waitv(const FutexWaitv* waiters, unsigned count) noexcept {
    IoAwaitable op = make_op(IoOpKind::FutexWaitv, -1);
    op.futex_waitv = waiters;
    op.length = count;
    return dispatch(op);
}

IoAwaitable epoll_ctl(dstd::NativeHandle epfd, int op_code,
                      dstd::NativeHandle fd,
                      struct epoll_event* event) noexcept {
    IoAwaitable op = make_op(IoOpKind::EpollCtl, epfd);
    op.epoll_events = event;
    op.flags = op_code;
    op.offset = fd;
    return dispatch(op);
}

IoAwaitable epoll_wait(dstd::NativeHandle epfd, struct epoll_event* events,
                       int max_events) noexcept {
    IoAwaitable op = make_op(IoOpKind::EpollWait, epfd);
    op.epoll_events = events;
    op.flags = max_events;
    return dispatch(op);
}
#endif

IoAwaitable call(BlockingFn fn, void* context) noexcept {
    IoAwaitable op = make_op(IoOpKind::Call, -1);
    op.call = fn;
    op.call_context = context;
    return dispatch(op);
}

IoAwaitable accept(dstd::NativeHandle listen_fd, struct sockaddr* addr,
                   socklen_t* addrlen) noexcept {
    IoAwaitable op = make_op(IoOpKind::Accept, listen_fd);
    op.addr = addr;
    op.addrlen = addrlen;
    if (IoBackend* backend = current_backend()) {
        op.backend = backend;
        op.resume_on = coro::Executor::current();
    } else {
        // A socket left blocking, unlike one a backend accepts: the blocking
        // recv and send that follow here must wait.
        op.result = ::accept(listen_fd, addr, addrlen);
        if (op.result < 0) op.result = -errno;
    }
    return op;
}

IoAwaitable recv(dstd::NativeHandle fd, void* buf, std::size_t len,
                 int flags) noexcept {
    return dispatch(make_buffer_op(IoOpKind::Recv, fd, buf, len, 0, flags));
}

IoAwaitable send(dstd::NativeHandle fd, const void* buf, std::size_t len,
                 int flags) noexcept {
    return dispatch(make_buffer_op(IoOpKind::Send, fd, const_cast<void*>(buf),
                                   len, 0, flags));
}

IoAwaitable readv(dstd::NativeHandle fd, const struct iovec* iov,
                  int iovcnt) noexcept {
    return dispatch(make_vector_op(IoOpKind::Readv, fd, iov, iovcnt));
}

IoAwaitable writev(dstd::NativeHandle fd, const struct iovec* iov,
                   int iovcnt) noexcept {
    return dispatch(make_vector_op(IoOpKind::Writev, fd, iov, iovcnt));
}

coro::CoroTask<ssize_t> writev_all(dstd::NativeHandle fd, struct iovec* iov,
                                   int iovcnt) {
    ssize_t total = 0;
    int i = 0;
    while (i < iovcnt) {
        int n = std::min(iovcnt - i, iov_max_entries());
        ssize_t rc = co_await runtime::io::writev(fd, iov + i, n);
        if (rc < 0) co_return rc;
        if (rc == 0) co_return -EIO;  // no progress; avoid spinning
        total += rc;

        auto written = static_cast<std::size_t>(rc);
        while (i < iovcnt && written > 0) {
            if (written >= iov[i].iov_len) {
                written -= iov[i].iov_len;
                ++i;
            } else {
                iov[i].iov_base = static_cast<char*>(iov[i].iov_base) + written;
                iov[i].iov_len -= written;
                written = 0;
            }
        }
    }
    co_return total;
}

IoAwaitable preadv(dstd::NativeHandle fd, const struct iovec* iov, int iovcnt,
                   std::uint64_t offset) noexcept {
    return dispatch(make_vector_op(IoOpKind::Preadv, fd, iov, iovcnt, offset));
}

IoAwaitable pwritev(dstd::NativeHandle fd, const struct iovec* iov, int iovcnt,
                    std::uint64_t offset) noexcept {
    return dispatch(make_vector_op(IoOpKind::Pwritev, fd, iov, iovcnt, offset));
}

IoAwaitable lseek(dstd::NativeHandle fd, std::int64_t offset,
                  int whence) noexcept {
    IoAwaitable op = make_op(IoOpKind::Lseek, fd);
    op.offset = offset;
    op.whence = whence;
    return dispatch(op);
}

IoAwaitable sendfile(dstd::NativeHandle out_fd, dstd::NativeHandle in_fd,
                     std::uint64_t offset, std::size_t count) noexcept {
    IoAwaitable op = make_op(IoOpKind::Sendfile, in_fd);
    op.out_handle = out_fd;
    op.offset = static_cast<std::int64_t>(offset);
    op.length = count;
    return dispatch(op);
}

}  // namespace dftracer::utils::runtime::io
