#ifndef DFTRACER_UTILS_TRACE_INTERNAL_CHUNK_LINE_SCANNER_H
#define DFTRACER_UTILS_TRACE_INTERNAL_CHUNK_LINE_SCANNER_H

#include <dftracer/utils/core/coro/task.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace dftracer::utils::trace::internal {

// Drive a byte-range stream and split each read chunk into newline-terminated
// lines, invoking `per_line(std::string_view line, std::uint32_t line_index)`
// for every non-empty line. `line_index` advances for every line segment
// (including empty and skipped ones), matching manifest line-number semantics.
//
// The line view aliases the stream's chunk buffer (zero-copy); `per_line` must
// not co_await and must be a callable that inlines (no std::function). Keeping
// the callback synchronous keeps this coroutine's frame minimal.
template <typename Stream, typename Fn>
dftracer::utils::coro::CoroTask<void> scan_chunk_lines(Stream& stream,
                                                       Fn&& per_line) {
    std::uint32_t line_index = 0;
    while (!stream.done()) {
        auto chunk = co_await stream.read_async();
        if (chunk.empty()) {
            break;
        }

        const std::size_t bytes_read = chunk.size();
        const char* data = chunk.data();
        std::size_t pos = 0;

        while (pos < bytes_read) {
            const char* line_start = data + pos;
            const char* newline = static_cast<const char*>(
                std::memchr(line_start, '\n', bytes_read - pos));

            if (!newline) {
                break;
            }

            const std::size_t line_len =
                static_cast<std::size_t>(newline - line_start);
            if (line_len > 0) {
                per_line(std::string_view(line_start, line_len), line_index);
            }

            pos = static_cast<std::size_t>(newline - data) + 1;
            ++line_index;
        }
    }
    co_return;
}

}  // namespace dftracer::utils::trace::internal

#endif  // DFTRACER_UTILS_TRACE_INTERNAL_CHUNK_LINE_SCANNER_H
