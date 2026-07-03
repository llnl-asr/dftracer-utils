#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_STREAMING_GZ_LINE_GENERATOR_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_STREAMING_GZ_LINE_GENERATOR_H

#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/exception_helpers.h>
#include <dftracer/utils/core/common/scoped_fd.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_decompressor_utility.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>

#include <string>
#include <vector>

namespace dftracer::utils::utilities::fileio::lines::sources {
/**
 * @brief Async generator that yields lines from .gz files without an index.
 *
 * Reads compressed chunks via async I/O, decompresses them through
 * StreamingDecompressorUtility (yielding zero-copy ByteView into the
 * decompressor's internal buffer), and splits the decompressed bytes
 * into lines. Zero heap allocations per read/decompress iteration.
 */
inline coro::AsyncGenerator<Line> async_streaming_gz_lines(
    std::string file_path, std::size_t start_line = 0,
    std::size_t end_line = 0) {
    ssize_t fd_result =
        co_await ::dftracer::utils::io::open(file_path.c_str(), O_RDONLY);
    if (fd_result < 0) {
        throw DFTUtilsException(
            ErrorCode::IO,
            "Cannot open compressed file: " + file_path + " (errno=" +
                std::to_string(static_cast<int>(-fd_result)) + ")");
    }
    dftracer::utils::ScopedFd fd(static_cast<int>(fd_result));

    constexpr std::size_t READ_BUFFER_SIZE = 256 * 1024;  // 256KB
    std::vector<char> read_buffer(READ_BUFFER_SIZE);
    std::string line_buffer;
    std::size_t current_line = 0;
    off_t file_offset = 0;

    compression::zlib::StreamingDecompressorUtility decompressor(
        compression::zlib::DecompressionFormat::AUTO);

    std::exception_ptr ex;

    try {
        while (true) {
            ssize_t bytes_read = co_await ::dftracer::utils::io::pread(
                fd.get(), read_buffer.data(), READ_BUFFER_SIZE, file_offset);

            if (bytes_read < 0) {
                throw DFTUtilsException(
                    ErrorCode::IO,
                    "Read error on compressed file: " + file_path + " (errno=" +
                        std::to_string(static_cast<int>(-bytes_read)) + ")");
            }

            if (bytes_read == 0) {
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

            // Pass read buffer directly as ByteView
            ByteView input(read_buffer.data(),
                           static_cast<std::size_t>(bytes_read));
            auto gen = decompressor.decompress(input);

            while (auto chunk = co_await gen.next()) {
                const char* data = chunk->as<char>();
                std::size_t remaining = chunk->size();
                std::size_t pos = 0;

                while (pos < remaining) {
                    const void* nl =
                        std::memchr(data + pos, '\n', remaining - pos);
                    if (nl) {
                        std::size_t nl_pos = static_cast<std::size_t>(
                            static_cast<const char*>(nl) - data);
                        if (nl_pos > pos) {
                            line_buffer.append(data + pos, nl_pos - pos);
                        }
                        current_line++;
                        if ((start_line == 0 || current_line >= start_line) &&
                            (end_line == 0 || current_line <= end_line)) {
                            co_yield Line(std::string_view(line_buffer),
                                          current_line);
                        }
                        if (end_line > 0 && current_line >= end_line) {
                            fd.reset();
                            co_return;
                        }
                        line_buffer.clear();
                        pos = nl_pos + 1;
                    } else {
                        line_buffer.append(data + pos, remaining - pos);
                        break;
                    }
                }
            }
        }
    } catch (...) {
        ex = std::current_exception();
    }

    if (ex) rethrow_and_clear(ex);
}

}  // namespace dftracer::utils::utilities::fileio::lines::sources

#endif
