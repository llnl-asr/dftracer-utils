#include <dftracer/utils/core/io/io_backend.h>
#include <dftracer/utils/core/io/io_sendfile.h>
#include <dftracer/utils/core/io/ops.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>

namespace dftracer::utils::io {

namespace {

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

}  // namespace

IoAwaitable read(int fd, void* buf, std::size_t len) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_read(fd, buf, len);
    }
    ssize_t result = ::read(fd, buf, len);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(result);
}

IoAwaitable write(int fd, const void* buf, std::size_t len) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_write(fd, buf, len);
    }
    ssize_t result = ::write(fd, buf, len);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(result);
}

IoAwaitable pread(int fd, void* buf, std::size_t len, off_t offset) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_pread(fd, buf, len, offset);
    }
    ssize_t result = ::pread(fd, buf, len, offset);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(result);
}

IoAwaitable pwrite(int fd, const void* buf, std::size_t len,
                   off_t offset) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_pwrite(fd, buf, len, offset);
    }
    ssize_t result = ::pwrite(fd, buf, len, offset);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(result);
}

IoAwaitable open(const char* path, int flags, mode_t mode) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_open(path, flags, mode);
    }
    int result = ::open(path, flags, mode);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(static_cast<ssize_t>(result));
}

IoAwaitable close(int fd) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_close(fd);
    }
    int result = ::close(fd);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(static_cast<ssize_t>(result));
}

IoAwaitable fsync(int fd) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_fsync(fd);
    }
    int result = ::fsync(fd);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(static_cast<ssize_t>(result));
}

IoAwaitable ftruncate(int fd, off_t length) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_ftruncate(fd, length);
    }
    int result = ::ftruncate(fd, length);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(static_cast<ssize_t>(result));
}

IoAwaitable fstat(int fd, struct stat* buf) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_fstat(fd, buf);
    }
    int result = ::fstat(fd, buf);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(static_cast<ssize_t>(result));
}

IoAwaitable accept(int listen_fd, struct sockaddr* addr,
                   socklen_t* addrlen) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_accept(listen_fd, addr, addrlen);
    }
    // Sync fallback
    int result = ::accept(listen_fd, addr, addrlen);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(static_cast<ssize_t>(result));
}

IoAwaitable recv(int fd, void* buf, std::size_t len, int flags) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_recv(fd, buf, len, flags);
    }
    ssize_t result = ::recv(fd, buf, len, flags);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(result);
}

IoAwaitable send(int fd, const void* buf, std::size_t len, int flags) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_send(fd, buf, len, flags);
    }
    ssize_t result = ::send(fd, buf, len, flags);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(result);
}

IoAwaitable readv(int fd, const struct iovec* iov, int iovcnt) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_readv(fd, iov, iovcnt);
    }
    ssize_t result = ::readv(fd, iov, iovcnt);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(result);
}

IoAwaitable writev(int fd, const struct iovec* iov, int iovcnt) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_writev(fd, iov, iovcnt);
    }
    ssize_t result = ::writev(fd, iov, iovcnt);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(result);
}

coro::CoroTask<ssize_t> writev_all(int fd, struct iovec* iov, int iovcnt) {
    ssize_t total = 0;
    int i = 0;
    while (i < iovcnt) {
        int n = std::min(iovcnt - i, iov_max_entries());
        ssize_t rc = co_await io::writev(fd, iov + i, n);
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

IoAwaitable preadv(int fd, const struct iovec* iov, int iovcnt,
                   off_t offset) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_preadv(fd, iov, iovcnt, offset);
    }
    ssize_t result = ::preadv(fd, iov, iovcnt, offset);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(result);
}

IoAwaitable pwritev(int fd, const struct iovec* iov, int iovcnt,
                    off_t offset) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_pwritev(fd, iov, iovcnt, offset);
    }
    ssize_t result = ::pwritev(fd, iov, iovcnt, offset);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(result);
}

IoAwaitable lseek(int fd, off_t offset, int whence) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_lseek(fd, offset, whence);
    }
    off_t result = ::lseek(fd, offset, whence);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(static_cast<ssize_t>(result));
}

IoAwaitable sendfile(int out_fd, int in_fd, off_t offset,
                     std::size_t count) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_sendfile(out_fd, in_fd, offset, count);
    }
    ssize_t result = platform_sendfile(out_fd, in_fd, offset, count);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(result);
}

}  // namespace dftracer::utils::io
