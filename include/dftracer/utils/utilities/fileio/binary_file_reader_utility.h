// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_BINARY_FILE_READER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_BINARY_FILE_READER_UTILITY_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>

#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <vector>

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
    const auto found = dstd::fs::exists(path.view());
    if (!found) throw common::DFTUtilsException(found.error());
    if (!*found) {
        throw common::DFTUtilsException(
            common::ErrorCode::NotFound,
            common::str_cat("File does not exist: ", path.str()));
    }

    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) {
        throw common::DFTUtilsException(
            common::ErrorCode::Io,
            common::str_cat("Cannot open file: ", path.str()));
    }

    std::vector<unsigned char> buffer(chunk_size);

    while (true) {
        file.read(reinterpret_cast<char*>(buffer.data()),
                  static_cast<std::streamsize>(chunk_size));
        std::streamsize bytes_read = file.gcount();

        if (bytes_read <= 0) break;

        co_yield dstd::ByteView(buffer.data(),
                                static_cast<std::size_t>(bytes_read));
    }

    if (file.bad()) {
        throw common::DFTUtilsException(
            common::ErrorCode::Io,
            common::str_cat("Error reading file: ", path.str()));
    }
}

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_BINARY_FILE_READER_UTILITY_H
