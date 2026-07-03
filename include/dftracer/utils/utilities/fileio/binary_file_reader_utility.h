#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_BINARY_FILE_READER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_BINARY_FILE_READER_UTILITY_H

#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/async_generator.h>

#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Streaming binary file reader yielding ByteView chunks.
 *
 * Reads a file in chunks and yields zero-copy ByteView references
 * into the internal read buffer. Each view is valid until the next
 * iteration.
 *
 * Usage:
 * @code
 * auto gen = read_binary_file("/path/to/file.bin");
 * while (auto chunk = co_await gen.next()) {
 *     process(chunk->as<char>(), chunk->size());
 * }
 * @endcode
 */
inline coro::AsyncGenerator<ByteView> read_binary_file(
    fs::path path, std::size_t chunk_size = 64 * 1024) {
    if (!fs::exists(path)) {
        throw DFTUtilsException(ErrorCode::NOT_FOUND,
                                "File does not exist: " + path.string());
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw DFTUtilsException(ErrorCode::IO,
                                "Cannot open file: " + path.string());
    }

    std::vector<unsigned char> buffer(chunk_size);

    while (true) {
        file.read(reinterpret_cast<char*>(buffer.data()),
                  static_cast<std::streamsize>(chunk_size));
        std::streamsize bytes_read = file.gcount();

        if (bytes_read <= 0) break;

        co_yield ByteView(buffer.data(), static_cast<std::size_t>(bytes_read));
    }

    if (file.bad()) {
        throw DFTUtilsException(ErrorCode::IO,
                                "Error reading file: " + path.string());
    }
}

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_BINARY_FILE_READER_UTILITY_H
