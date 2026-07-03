#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_FILE_SOURCE_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_FILE_SOURCE_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/scoped_fd.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>

#include <string>

namespace dftracer::utils::utilities::fileio::lines::sources {

// Open a file read-only via async I/O, throwing DFTUtilsException(IO) on
// failure. Returns an owning ScopedFd.
inline coro::CoroTask<ScopedFd> async_open_read(const std::string& file_path) {
    ssize_t fd =
        co_await ::dftracer::utils::io::open(file_path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw DFTUtilsException(ErrorCode::IO,
                                "Cannot open file: " + file_path);
    }
    co_return ScopedFd(static_cast<int>(fd));
}

// Build the IO read-error for a failed pread. neg_errno is the negated errno
// returned by io::pread (bytes_read < 0).
inline DFTUtilsException make_read_error(const std::string& file_path,
                                         ssize_t neg_errno) {
    return DFTUtilsException(
        ErrorCode::IO, "Read error on file: " + file_path + " (errno=" +
                           std::to_string(static_cast<int>(-neg_errno)) + ")");
}

}  // namespace dftracer::utils::utilities::fileio::lines::sources

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_FILE_SOURCE_H
