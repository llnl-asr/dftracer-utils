#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_PLAIN_FILE_BYTES_GENERATOR_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_PLAIN_FILE_BYTES_GENERATOR_H

#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>

#include <string>
#include <vector>

namespace dftracer::utils::utilities::fileio::lines::sources {

/**
 * @brief Async generator that yields lines from plain text files
 *        within a byte range, with line-boundary alignment.
 *
 * Mirrors PlainFileBytesIterator but uses async I/O. When start_byte
 * is non-zero, skips forward to the next newline to avoid partial lines.
 * Reads strictly until end_byte (may truncate the last line).
 *
 * Usage:
 * @code
 * auto gen = async_plain_file_bytes("data.txt", 1000, 5000);
 * while (auto line = co_await gen.next()) {
 *     process(*line);
 * }
 * @endcode
 */
inline coro::AsyncGenerator<Line> async_plain_file_bytes(
    std::string file_path, std::size_t start_byte, std::size_t end_byte,
    std::size_t buffer_size = 256 * 1024) {
    if (start_byte >= end_byte) {
        throw std::invalid_argument("Invalid byte range: start >= end");
    }

    // Open file asynchronously
    ssize_t fd_result =
        co_await ::dftracer::utils::io::open(file_path.c_str(), O_RDONLY);
    if (fd_result < 0) {
        throw std::runtime_error("Cannot open file: " + file_path);
    }
    int fd = static_cast<int>(fd_result);

    std::vector<char> read_buffer(buffer_size);
    std::string line_buffer;
    std::size_t current_line = 0;
    auto file_offset = static_cast<off_t>(start_byte);

    // Align to next line boundary if starting mid-file
    if (start_byte > 0) {
        // Read a chunk to find the next newline
        bool aligned = false;
        while (!aligned) {
            ssize_t bytes_read = co_await ::dftracer::utils::io::pread(
                fd, read_buffer.data(), read_buffer.size(), file_offset);

            if (bytes_read <= 0) {
                // Hit EOF before finding a newline — nothing to yield
                co_await ::dftracer::utils::io::close(fd);
                co_return;
            }

            for (ssize_t i = 0; i < bytes_read; ++i) {
                file_offset++;
                if (read_buffer[static_cast<std::size_t>(i)] == '\n') {
                    aligned = true;
                    break;
                }
            }

            if (static_cast<std::size_t>(file_offset) >= end_byte) {
                // Passed end_byte while aligning — nothing to yield
                co_await ::dftracer::utils::io::close(fd);
                co_return;
            }
        }
    }

    // Read chunks and split into lines.
    // Mirrors sync PlainFileBytesIterator: process chars while pos < end_byte.
    bool done = false;
    while (!done) {
        // Check if we've reached the end boundary before reading
        if (static_cast<std::size_t>(file_offset) >= end_byte) {
            break;
        }

        ssize_t bytes_read = co_await ::dftracer::utils::io::pread(
            fd, read_buffer.data(), read_buffer.size(), file_offset);

        if (bytes_read <= 0) {
            // EOF — yield final partial line if any
            if (!line_buffer.empty()) {
                current_line++;
                co_yield Line(std::string_view(line_buffer), current_line);
            }
            done = true;
            break;
        }

        for (ssize_t i = 0; i < bytes_read; ++i) {
            auto abs_pos = static_cast<std::size_t>(file_offset) +
                           static_cast<std::size_t>(i);

            // Stop strictly at end_byte (sync reads while pos < end)
            if (abs_pos >= end_byte) {
                // Yield whatever partial content we accumulated
                // But only if we have content (sync: has_line=true only if
                // buffer not empty)
                if (!line_buffer.empty()) {
                    current_line++;
                    co_yield Line(std::string_view(line_buffer), current_line);
                }
                done = true;
                break;
            }

            char c = read_buffer[static_cast<std::size_t>(i)];

            if (c == '\n') {
                // Sync yields a line when it finds \n, even if buffer
                // is empty (empty line).
                // After yielding, check if we've passed end_byte.
                current_line++;
                co_yield Line(std::string_view(line_buffer), current_line);
                line_buffer.clear();

                // Check if we've reached end_byte after consuming the newline
                if (abs_pos + 1 >= end_byte) {
                    done = true;
                    break;
                }
            } else {
                line_buffer.push_back(c);
            }
        }

        file_offset += bytes_read;
    }

    co_await ::dftracer::utils::io::close(fd);
}

}  // namespace dftracer::utils::utilities::fileio::lines::sources

#endif