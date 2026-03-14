#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_PLAIN_FILE_LINE_GENERATOR_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_PLAIN_FILE_LINE_GENERATOR_H

#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>

#include <string>
#include <vector>

namespace dftracer::utils::utilities::fileio::lines::sources {

/**
 * @brief Async generator that yields lines from plain text files.
 *
 * Uses io::open + io::pread for non-blocking I/O.
 *
 * Usage:
 * @code
 * auto gen = async_plain_file_lines("data.txt");
 * while (auto line = co_await gen.next()) {
 *     process(*line);
 * }
 * @endcode
 */
inline coro::AsyncGenerator<Line> async_plain_file_lines(
    std::string file_path, std::size_t start_line = 0,
    std::size_t end_line = 0) {
    // Open file asynchronously
    ssize_t fd_result =
        co_await ::dftracer::utils::io::open(file_path.c_str(), O_RDONLY);
    if (fd_result < 0) {
        throw std::runtime_error("Cannot open file: " + file_path);
    }
    int fd = static_cast<int>(fd_result);

    constexpr std::size_t BUFFER_SIZE = 256 * 1024;  // 256KB
    std::vector<char> read_buffer(BUFFER_SIZE);
    std::string line_buffer;
    std::size_t current_line = 0;
    off_t file_offset = 0;

    // Capture exceptions so we can close fd before rethrowing
    // (co_await is not allowed inside catch handlers).
    std::exception_ptr ex;

    try {
        bool eof = false;
        while (!eof) {
            ssize_t bytes_read = co_await ::dftracer::utils::io::pread(
                fd, read_buffer.data(), BUFFER_SIZE, file_offset);

            if (bytes_read < 0) {
                throw std::runtime_error(
                    "Read error on file: " + file_path + " (errno=" +
                    std::to_string(static_cast<int>(-bytes_read)) + ")");
            }

            if (bytes_read == 0) {
                // EOF — yield final line if buffer has content
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

            // Parse lines from buffer
            for (ssize_t i = 0; i < bytes_read; ++i) {
                char c = read_buffer[static_cast<std::size_t>(i)];
                if (c == '\n') {
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
                    line_buffer.push_back(c);
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
