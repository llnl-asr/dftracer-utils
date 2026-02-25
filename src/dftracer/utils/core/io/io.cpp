#include <dftracer/utils/core/io/io_backend.h>
#include <dftracer/utils/core/io/ops.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>

namespace dftracer::utils::io {

IoAwaitable read(int fd, void* buf, std::size_t len, off_t offset) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_read(fd, buf, len, offset);
    }
    // Sync fallback
    ssize_t result = ::pread(fd, buf, len, offset);
    if (result < 0) result = -errno;
    return IoAwaitable::ready(result);
}

IoAwaitable write(int fd, const void* buf, std::size_t len,
                  off_t offset) noexcept {
    auto* exec = Executor::current();
    if (exec && exec->has_io_backend()) {
        return exec->io_backend().submit_write(fd, buf, len, offset);
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

}  // namespace dftracer::utils::io
