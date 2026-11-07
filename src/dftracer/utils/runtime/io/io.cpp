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
