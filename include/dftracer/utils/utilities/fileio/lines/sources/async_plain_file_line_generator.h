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
 * Uses io::open + io::read for non-blocking I/O.
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

    // Read chunks and split into lines
    bool eof = false;
    while (!eof) {
        // Async read
        ssize_t bytes_read = co_await ::dftracer::utils::io::read(
            fd, read_buffer.data(), BUFFER_SIZE, file_offset);

        if (bytes_read <= 0) {
            eof = true;
            // Yield final line if buffer has content
            if (!line_buffer.empty()) {
                current_line++;
                if ((start_line == 0 || current_line >= start_line) &&
                    (end_line == 0 || current_line <= end_line)) {
                    co_yield Line(std::string_view(line_buffer), current_line);
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
                    co_yield Line(std::string_view(line_buffer), current_line);
                }
                if (end_line > 0 && current_line >= end_line) {
                    // Close fd and exit
                    co_await ::dftracer::utils::io::close(fd);
                    co_return;
                }
                line_buffer.clear();
            } else {
                line_buffer.push_back(c);
            }
        }
    }

    co_await ::dftracer::utils::io::close(fd);
}

}  // namespace dftracer::utils::utilities::fileio::lines::sources

#endif
