// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/io/io_sendfile.h>
#include <dftracer/utils/runtime/io/ops.h>
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

// Every free op has the same shape: if the executor driving this thread has a
// backend, forward to its submit_* member (async); else run the blocking
// syscall, fold a failure to -errno, and hand back a ready awaitable. Submit is
// the backend member; sync is the syscall wrapper.
template <auto Submit, class Sync, class... Args>
IoAwaitable io_dispatch(Sync sync, Args... args) noexcept {
    if (auto* backend = current_backend()) {
        return (backend->*Submit)(args...);
    }
    auto result = sync(args...);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(static_cast<ssize_t>(result));
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
    return io_dispatch<&IoBackend::submit_read>(
        [](int f, void* b, std::size_t n) { return ::read(f, b, n); }, fd, buf,
        len);
}

IoAwaitable write(dstd::NativeHandle fd, const void* buf,
                  std::size_t len) noexcept {
    return io_dispatch<&IoBackend::submit_write>(
        [](int f, const void* b, std::size_t n) { return ::write(f, b, n); },
        fd, buf, len);
}

IoAwaitable pread(dstd::NativeHandle fd, void* buf, std::size_t len,
                  std::uint64_t offset) noexcept {
    return io_dispatch<&IoBackend::submit_pread>(
        [](int f, void* b, std::size_t n, off_t o) {
            return ::pread(f, b, n, o);
        },
        fd, buf, len, static_cast<off_t>(offset));
}

IoAwaitable pwrite(dstd::NativeHandle fd, const void* buf, std::size_t len,
                   std::uint64_t offset) noexcept {
    return io_dispatch<&IoBackend::submit_pwrite>(
        [](int f, const void* b, std::size_t n, off_t o) {
            return ::pwrite(f, b, n, o);
        },
        fd, buf, len, static_cast<off_t>(offset));
}

IoAwaitable open(const char* path, int flags, mode_t mode) noexcept {
    return io_dispatch<&IoBackend::submit_open>(
        [](const char* p, int fl, mode_t m) { return ::open(p, fl, m); }, path,
        flags, mode);
}

IoAwaitable close(dstd::NativeHandle fd) noexcept {
    return io_dispatch<&IoBackend::submit_close>(
        [](int f) { return ::close(f); }, fd);
}

IoAwaitable fsync(dstd::NativeHandle fd) noexcept {
    return io_dispatch<&IoBackend::submit_fsync>(
        [](int f) { return ::fsync(f); }, fd);
}

IoAwaitable ftruncate(dstd::NativeHandle fd, std::uint64_t length) noexcept {
    return io_dispatch<&IoBackend::submit_ftruncate>(
        [](int f, off_t l) { return ::ftruncate(f, l); }, fd, length);
}

IoAwaitable fstat(dstd::NativeHandle fd, struct stat* buf) noexcept {
    return io_dispatch<&IoBackend::submit_fstat>(
        [](int f, struct stat* b) { return ::fstat(f, b); }, fd, buf);
}

IoAwaitable accept(dstd::NativeHandle listen_fd, struct sockaddr* addr,
                   socklen_t* addrlen) noexcept {
    return io_dispatch<&IoBackend::submit_accept>(
        [](int lf, struct sockaddr* a, socklen_t* al) {
            return ::accept(lf, a, al);
        },
        listen_fd, addr, addrlen);
}

IoAwaitable recv(dstd::NativeHandle fd, void* buf, std::size_t len,
                 int flags) noexcept {
    return io_dispatch<&IoBackend::submit_recv>(
        [](int f, void* b, std::size_t n, int fl) {
            return ::recv(f, b, n, fl);
        },
        fd, buf, len, flags);
}

IoAwaitable send(dstd::NativeHandle fd, const void* buf, std::size_t len,
                 int flags) noexcept {
    return io_dispatch<&IoBackend::submit_send>(
        [](int f, const void* b, std::size_t n, int fl) {
            return ::send(f, b, n, fl);
        },
        fd, buf, len, flags);
}

IoAwaitable readv(dstd::NativeHandle fd, const struct iovec* iov,
                  int iovcnt) noexcept {
    return io_dispatch<&IoBackend::submit_readv>(
        [](int f, const struct iovec* v, int c) { return ::readv(f, v, c); },
        fd, iov, iovcnt);
}

IoAwaitable writev(dstd::NativeHandle fd, const struct iovec* iov,
                   int iovcnt) noexcept {
    return io_dispatch<&IoBackend::submit_writev>(
        [](int f, const struct iovec* v, int c) { return ::writev(f, v, c); },
        fd, iov, iovcnt);
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
    return io_dispatch<&IoBackend::submit_preadv>(
        [](int f, const struct iovec* v, int c, off_t o) {
            return ::preadv(f, v, c, o);
        },
        fd, iov, iovcnt, static_cast<off_t>(offset));
}

IoAwaitable pwritev(dstd::NativeHandle fd, const struct iovec* iov, int iovcnt,
                    std::uint64_t offset) noexcept {
    return io_dispatch<&IoBackend::submit_pwritev>(
        [](int f, const struct iovec* v, int c, off_t o) {
            return ::pwritev(f, v, c, o);
        },
        fd, iov, iovcnt, static_cast<off_t>(offset));
}

IoAwaitable lseek(dstd::NativeHandle fd, std::int64_t offset,
                  int whence) noexcept {
    return io_dispatch<&IoBackend::submit_lseek>(
        [](int f, off_t o, int w) { return ::lseek(f, o, w); }, fd,
        static_cast<off_t>(offset), whence);
}

IoAwaitable sendfile(dstd::NativeHandle out_fd, dstd::NativeHandle in_fd,
                     std::uint64_t offset, std::size_t count) noexcept {
    return io_dispatch<&IoBackend::submit_sendfile>(
        [](int of, int inf, off_t o, std::size_t c) {
            return platform_sendfile(of, inf, o, c);
        },
        out_fd, in_fd, static_cast<off_t>(offset), count);
}

}  // namespace dftracer::utils::runtime::io
