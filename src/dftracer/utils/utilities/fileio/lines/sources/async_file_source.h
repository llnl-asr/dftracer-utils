// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_FILE_SOURCE_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_FILE_SOURCE_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/runtime/io/io.h>

namespace dftracer::utils::utilities::fileio::lines::sources {

/// Open a file read-only via async I/O, throwing DFTUtilsException(IO) on
/// failure. Returns an owning @ref dftracer::utils::dstd::File "File".
inline coro::CoroTask<dstd::File> async_open_read(dstd::StringView file_path) {
    ssize_t fd = co_await ::dftracer::utils::runtime::io::open(
        file_path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw common::DFTUtilsException(
            common::make_error<"Cannot open file: {}">(common::ErrorCode::Io,
                                                       file_path)
                .error());
    }
    co_return dstd::File::from_native_handle(static_cast<int>(fd));
}

/// Build the IO read-error for a failed pread. neg_errno is the negated errno
/// returned by runtime::io::pread (bytes_read < 0).
inline common::DFTUtilsException make_read_error(dstd::StringView file_path,
                                                 ssize_t neg_errno) {
    return common::DFTUtilsException(
        common::make_error<"Read error on file: {} (errno={})">(
            common::ErrorCode::Io, file_path, static_cast<int>(-neg_errno))
            .error());
}

}  // namespace dftracer::utils::utilities::fileio::lines::sources

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_FILE_SOURCE_H
