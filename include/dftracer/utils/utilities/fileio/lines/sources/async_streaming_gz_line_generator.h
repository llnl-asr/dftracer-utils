#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_STREAMING_GZ_LINE_GENERATOR_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_STREAMING_GZ_LINE_GENERATOR_H

#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_decompressor_utility.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>
#include <dftracer/utils/utilities/fileio/types/compressed_data.h>

#include <string>
#include <vector>

namespace dftracer::utils::utilities::fileio::lines::sources {

/**
 * @brief Async generator that yields lines from .gz files without an index.
 *
 * Reads compressed chunks via async I/O, decompresses them through
 * StreamingDecompressorUtility, and splits the decompressed bytes into
 * lines. This avoids the double-decompression overhead of building a
 * sidecar index first.
 *
 * Usage:
 * @code
 * auto gen = async_streaming_gz_lines("data.pfw.gz");
 * while (auto line = co_await gen.next()) {
 *     process(*line);
 * }
 * @endcode
 */
inline coro::AsyncGenerator<Line> async_streaming_gz_lines(
    std::string file_path, std::size_t start_line = 0,
    std::size_t end_line = 0) {
    ssize_t fd_result =
        co_await ::dftracer::utils::io::open(file_path.c_str(), O_RDONLY);
    if (fd_result < 0) {
        throw std::runtime_error(
            "Cannot open compressed file: " + file_path +
            " (errno=" + std::to_string(static_cast<int>(-fd_result)) + ")");
    }
    int fd = static_cast<int>(fd_result);

    constexpr std::size_t READ_BUFFER_SIZE = 256 * 1024;  // 256KB
    std::vector<char> read_buffer(READ_BUFFER_SIZE);
    std::string line_buffer;
    std::size_t current_line = 0;
    off_t file_offset = 0;

    compression::zlib::StreamingDecompressorUtility decompressor(
        compression::zlib::DecompressionFormat::AUTO);

    // Capture exceptions so we can close fd before rethrowing
    // (co_await is not allowed inside catch handlers).
    std::exception_ptr ex;

    try {
        bool eof = false;
        while (!eof) {
            ssize_t bytes_read = co_await ::dftracer::utils::io::pread(
                fd, read_buffer.data(), READ_BUFFER_SIZE, file_offset);

            if (bytes_read < 0) {
                throw std::runtime_error(
                    "Read error on compressed file: " + file_path + " (errno=" +
                    std::to_string(static_cast<int>(-bytes_read)) + ")");
            }

            if (bytes_read == 0) {
                // EOF — yield final partial line if any
                if (!line_buffer.empty()) {
                    current_line++;
                    if ((start_line == 0 || current_line >= start_line) &&
                        (end_line == 0 || current_line <= end_line)) {
                        co_yield Line(std::string_view(line_buffer),
                                      current_line);
                    }
                }
                break;
            }

            file_offset += bytes_read;

            // Wrap raw bytes into CompressedData for the decompressor
            CompressedData compressed(std::vector<unsigned char>(
                reinterpret_cast<unsigned char*>(read_buffer.data()),
                reinterpret_cast<unsigned char*>(read_buffer.data()) +
                    bytes_read));

            auto raw_chunks = co_await decompressor.process(compressed);

            // Split decompressed bytes into lines
            for (const auto& raw : raw_chunks) {
                for (unsigned char byte : raw.data) {
                    if (byte == '\n') {
                        current_line++;
                        if ((start_line == 0 || current_line >= start_line) &&
                            (end_line == 0 || current_line <= end_line)) {
                            co_yield Line(std::string_view(line_buffer),
                                          current_line);
                        }
                        if (end_line > 0 && current_line >= end_line) {
                            co_await ::dftracer::utils::io::close(fd);
                            co_return;
                        }
                        line_buffer.clear();
                    } else {
                        line_buffer.push_back(static_cast<char>(byte));
                    }
                }
            }
        }
    } catch (...) {
        ex = std::current_exception();
    }

    co_await ::dftracer::utils::io::close(fd);
    if (ex) {
        std::rethrow_exception(ex);
    }
}

}  // namespace dftracer::utils::utilities::fileio::lines::sources

#endif
