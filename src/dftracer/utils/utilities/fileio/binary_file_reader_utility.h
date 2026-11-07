// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_BINARY_FILE_READER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_BINARY_FILE_READER_UTILITY_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/io/ops.h>

#include <cerrno>
#include <cstddef>
#include <utility>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Streaming binary file reader yielding @ref
 * dftracer::utils::dstd::ByteView "ByteView" chunks.
 *
 * Reads a file in chunks and yields zero-copy @ref
 * dftracer::utils::dstd::ByteView "ByteView" references into the internal read
 * buffer. Each view is valid until the next iteration.
 *
 * Usage:
 * @code
 * auto gen = read_binary_file("/path/to/file.bin");
 * for (;;) {
 *     auto chunk = co_await gen.next();
 *     if (!chunk) break;
 *     process(chunk->as<char>(), chunk->size());
 * }
 * @endcode
 */
inline coro::AsyncGenerator<dstd::ByteView> read_binary_file(
    dstd::Path path, std::size_t chunk_size = 64 * 1024) {
    auto checking = runtime::fs::exists(path.view());
    const auto found = co_await std::move(checking);
    if (!found) throw common::DFTUtilsException(found.error());
    if (!*found) {
        throw common::DFTUtilsException(
            common::ErrorCode::NotFound,
            common::str_cat("File does not exist: ", path.str()));
    }

    const dstd::OpenOptions open_options;
    auto opening = runtime::io::open_file(path.view(), open_options);
    auto opened = co_await std::move(opening);
    if (!opened) {
        throw common::DFTUtilsException(
            common::ErrorCode::Io,
            common::str_cat("Cannot open file: ", path.str()));
    }
    const dstd::File file = std::move(*opened);

    dstd::Vector<unsigned char> buffer(chunk_size);

    while (true) {
        // Sequential, as the path may be a FIFO.
        const ssize_t bytes_read = co_await runtime::io::read(
            file.native_handle(), buffer.data(), chunk_size);
        if (bytes_read == -EINTR) continue;
        if (bytes_read < 0) {
            throw common::DFTUtilsException(
                common::ErrorCode::Io,
                common::str_cat("Error reading file: ", path.str()));
        }

        if (bytes_read == 0) break;

        co_yield dstd::ByteView(buffer.data(),
                                static_cast<std::size_t>(bytes_read));
    }
}

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_BINARY_FILE_READER_UTILITY_H
