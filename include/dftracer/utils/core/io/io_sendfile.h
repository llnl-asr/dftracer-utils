#ifndef DFTRACER_UTILS_CORE_IO_IO_SENDFILE_H
#define DFTRACER_UTILS_CORE_IO_IO_SENDFILE_H

#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>

#ifdef __linux__
#include <sys/sendfile.h>
#elif defined(__APPLE__)
#include <sys/socket.h>
#include <sys/uio.h>
#endif

namespace dftracer::utils::io {

// Copy `count` bytes from `in_fd` at `offset` to `out_fd`, using the platform's
// native sendfile where available and a portable pread+write loop otherwise.
// Returns the number of bytes transferred, or a negative value on error (the
// caller maps a negative return to -errno).
inline ssize_t platform_sendfile(int out_fd, int in_fd, off_t offset,
                                 std::size_t count) {
#ifdef __linux__
    off_t off = offset;
    return ::sendfile(out_fd, in_fd, &off, count);
#elif defined(__APPLE__)
    off_t len = static_cast<off_t>(count);
    int ret = ::sendfile(in_fd, out_fd, offset, &len, nullptr, 0);
    return (ret == 0 || errno == EAGAIN) ? len : -1;
#else
    char tmp[8192];
    ssize_t result = 0;
    off_t off = offset;
    std::size_t remaining = count;
    while (remaining > 0) {
        std::size_t chunk = remaining < sizeof(tmp) ? remaining : sizeof(tmp);
        ssize_t r = ::pread(in_fd, tmp, chunk, off);
        if (r <= 0) {
            if (result == 0) result = r;
            break;
        }
        ssize_t w = ::write(out_fd, tmp, static_cast<std::size_t>(r));
        if (w < 0) {
            if (result == 0) result = w;
            break;
        }
        result += w;
        off += w;
        remaining -= static_cast<std::size_t>(w);
        if (w < r) break;
    }
    return result;
#endif
}

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_CORE_IO_IO_SENDFILE_H
