// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_FILEIO_LINES_SOURCES_ASYNC_STREAMING_PLAIN_LINE_GENERATOR_H
#define DFTRACER_UTILS_FILEIO_LINES_SOURCES_ASYNC_STREAMING_PLAIN_LINE_GENERATOR_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/fileio/binary_file_reader_utility.h>
#include <dftracer/utils/fileio/lines/line_types.h>

#include <cstddef>
#include <cstring>

namespace dftracer::utils::fileio::lines::sources {

/**
 * @brief Async generator that yields lines from a plain (uncompressed) file.
 *
 * Reads the file in chunks of `chunk_size` bytes. A line inside one chunk is
 * yielded in place, as a view into the chunk; only a line cut by a chunk
 * boundary is put together in a buffer. Each view is valid until the next
 * call to next(). Line numbers are 1-based. `end_line` 0 means to the end of
 * the file; the last line is yielded without a trailing newline.
 *
 * A failure (a missing file, an open or a read error) is yielded as an error
 * item, after which the generator ends. The lines before it were yielded
 * first.
 */
inline coro::AsyncGenerator<dstd::Result<Line>> async_streaming_plain_lines(
    dstd::String file_path, std::size_t start_line = 0,
    std::size_t end_line = 0, std::size_t chunk_size = std::size_t{1} << 20) {
    auto chunks = read_binary_file(dstd::Path(file_path), chunk_size);
    dstd::String line_buffer;
    std::size_t current_line = 0;

    for (;;) {
        auto chunk = co_await chunks.next();
        if (!chunk) break;
        if (!*chunk) {
            co_yield dstd::Result<Line>(chunk->error());
            co_return;
        }
        const char* data = (*chunk)->as<char>();
        const std::size_t remaining = (*chunk)->size();
        std::size_t pos = 0;
        while (pos < remaining) {
            const void* nl = std::memchr(data + pos, '\n', remaining - pos);
            if (!nl) {
                line_buffer.append(data + pos, remaining - pos);
                break;
            }
            const std::size_t nl_pos =
                static_cast<std::size_t>(static_cast<const char*>(nl) - data);
            dstd::StringView line(data + pos, nl_pos - pos);
            if (!line_buffer.empty()) {
                line_buffer.append(line);
                line = line_buffer;
            }
            current_line++;
            if ((start_line == 0 || current_line >= start_line) &&
                (end_line == 0 || current_line <= end_line)) {
                co_yield dstd::Result<Line>(Line(line, current_line));
            }
            if (end_line > 0 && current_line >= end_line) co_return;
            line_buffer.clear();
            pos = nl_pos + 1;
        }
    }
    if (!line_buffer.empty()) {
        current_line++;
        if ((start_line == 0 || current_line >= start_line) &&
            (end_line == 0 || current_line <= end_line)) {
            co_yield dstd::Result<Line>(
                Line(dstd::StringView(line_buffer), current_line));
        }
    }
}

}  // namespace dftracer::utils::fileio::lines::sources

#endif
