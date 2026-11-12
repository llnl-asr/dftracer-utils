// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_FILEIO_LINES_SOURCES_ASYNC_STREAMING_GZ_LINE_GENERATOR_H
#define DFTRACER_UTILS_FILEIO_LINES_SOURCES_ASYNC_STREAMING_GZ_LINE_GENERATOR_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/fileio/compress/gzip_member_reader.h>
#include <dftracer/utils/fileio/lines/line_types.h>
#include <dftracer/utils/runtime/io/io.h>

#include <cstring>

namespace dftracer::utils::fileio::lines::sources {
/**
 * @brief Async generator that yields lines from .gz files without an index.
 *
 * Decodes the file a gzip member at a time (peak memory one member). A member
 * too large to hold whole is streamed by lines. A last member cut short yields
 * its complete lines, or is an error when `recover_truncated` is false.
 * `open_file`, when given, replaces `file_path` and closes with its last owner.
 *
 * A failure (open, stat, read or a corrupt or cut member) is yielded as an
 * error item, after which the generator ends. The lines before it were
 * yielded first.
 */
inline coro::AsyncGenerator<dstd::Result<Line>> async_streaming_gz_lines(
    dstd::String file_path, std::size_t start_line = 0,
    std::size_t end_line = 0, bool recover_truncated = true,
    dftracer::utils::dstd::SharedPtr<dftracer::utils::dstd::File> open_file =
        nullptr) {
    dftracer::utils::dstd::File owned;
    if (!open_file) {
        ssize_t fd_result = co_await ::dftracer::utils::runtime::io::open(
            file_path.c_str(), O_RDONLY);
        if (fd_result < 0) {
            co_yield dstd::Result<Line>(
                common::make_error<
                    "Cannot open compressed file: {} (errno={})">(
                    common::ErrorCode::Io, file_path,
                    static_cast<int>(-fd_result)));
            co_return;
        }
        owned = dftracer::utils::dstd::File::from_native_handle(
            static_cast<int>(fd_result));
    }
    dftracer::utils::dstd::File& fd = open_file ? *open_file : owned;

    const auto st = fd.stat();
    if (!st) {
        co_yield dstd::Result<Line>(
            common::make_error<"Cannot stat compressed file: {}">(
                common::ErrorCode::Io, file_path));
        co_return;
    }
    const std::uint64_t file_size = st->size;

    dstd::String line_buffer;
    std::size_t current_line = 0;

    {
        auto bytes = compress::decode_gzip_members(
            fd.native_handle(), file_size, std::size_t{32} << 20,
            recover_truncated);
        for (;;) {
            auto chunk = co_await bytes.next();
            if (!chunk) break;
            if (!*chunk) {
                co_yield dstd::Result<Line>(chunk->error());
                co_return;
            }
            const char* data = (*chunk)->data();
            const std::size_t remaining = (*chunk)->size();
            std::size_t pos = 0;
            while (pos < remaining) {
                const void* nl = std::memchr(data + pos, '\n', remaining - pos);
                if (nl) {
                    const std::size_t nl_pos = static_cast<std::size_t>(
                        static_cast<const char*>(nl) - data);
                    // A line inside the member is yielded in place; only one
                    // cut by a member boundary is put together in line_buffer.
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
                    if (end_line > 0 && current_line >= end_line) {
                        owned.close().discard();
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
        if (!line_buffer.empty()) {
            current_line++;
            if ((start_line == 0 || current_line >= start_line) &&
                (end_line == 0 || current_line <= end_line)) {
                co_yield dstd::Result<Line>(
                    Line(dstd::StringView(line_buffer), current_line));
            }
        }
    }
}

}  // namespace dftracer::utils::fileio::lines::sources

#endif
