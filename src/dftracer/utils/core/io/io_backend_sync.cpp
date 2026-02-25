#include <dftracer/utils/core/io/io_backend.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

namespace dftracer::utils::io {

ssize_t IoBackend::submit_read_sync(int fd, void *buf, std::size_t len,
                                    off_t offset) {
    // For sync wrappers, we cannot use the normal IoAwaitable coroutine
    // path. Instead, we directly call the POSIX syscall. The VFS runs
    // on a dedicated SQLite thread (not an executor worker), so blocking
    // is acceptable.
    ssize_t result = ::pread(fd, buf, len, offset);
    if (result < 0) result = -errno;
    return result;
}

ssize_t IoBackend::submit_write_sync(int fd, const void *buf, std::size_t len,
                                     off_t offset) {
    ssize_t result = ::pwrite(fd, const_cast<void *>(buf), len, offset);
    if (result < 0) result = -errno;
    return result;
}

int IoBackend::submit_fsync_sync(int fd) {
    int result = ::fsync(fd);
    if (result < 0) result = -errno;
    return result;
}

int IoBackend::submit_ftruncate_sync(int fd, off_t length) {
    int result = ::ftruncate(fd, length);
    if (result < 0) result = -errno;
    return result;
}

int IoBackend::submit_fstat_sync(int fd, struct stat *buf) {
    int result = ::fstat(fd, buf);
    if (result < 0) result = -errno;
    return result;
}

}  // namespace dftracer::utils::io
