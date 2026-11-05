// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_STREAMING_GZ_LINE_GENERATOR_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_STREAMING_GZ_LINE_GENERATOR_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_member_reader.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>
#include <sys/stat.h>

#include <cstring>

namespace dftracer::utils::utilities::fileio::lines::sources {
/**
 * @brief Async generator that yields lines from .gz files without an index.
 *
 * Decodes the file a gzip member at a time (peak memory one member). A member
 * too large to hold whole is streamed by lines. A last member cut short yields
 * its complete lines, or throws when `recover_truncated` is false. `open_file`,
 * when given, replaces `file_path` and closes with its last owner.
 */
inline coro::AsyncGenerator<Line> async_streaming_gz_lines(
    dstd::String file_path, std::size_t start_line = 0,
    std::size_t end_line = 0, bool recover_truncated = true,
    dftracer::utils::dstd::SharedPtr<dftracer::utils::dstd::ScopedFd>
        open_file = nullptr) {
    dftracer::utils::dstd::ScopedFd owned;
    if (!open_file) {
        ssize_t fd_result = co_await ::dftracer::utils::runtime::io::open(
            file_path.c_str(), O_RDONLY);
        if (fd_result < 0) {
            throw common::DFTUtilsException(
                common::ErrorCode::Io,
                "Cannot open compressed file: " + file_path + " (errno=" +
                    dstd::to_string(static_cast<int>(-fd_result)) + ")");
        }
        owned = dftracer::utils::dstd::ScopedFd(static_cast<int>(fd_result));
    }
    dftracer::utils::dstd::ScopedFd& fd = open_file ? *open_file : owned;

    struct stat st;
    if (::fstat(fd.get(), &st) != 0) {
        throw common::DFTUtilsException(
            common::ErrorCode::Io, "Cannot stat compressed file: " + file_path);
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(st.st_size);

    dstd::String line_buffer;
    std::size_t current_line = 0;
    std::exception_ptr ex;

    try {
        auto bytes = compress::decode_gzip_members(
            fd.get(), file_size, std::size_t{32} << 20, recover_truncated);
        for (;;) {
            auto chunk = co_await bytes.next();
            if (!chunk) break;
            const char* data = chunk->data();
            const std::size_t remaining = chunk->size();
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
                        co_yield Line(line, current_line);
                    }
                    if (end_line > 0 && current_line >= end_line) {
                        owned.reset();
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
                co_yield Line(dstd::StringView(line_buffer), current_line);
            }
        }
    } catch (...) {
        ex = std::current_exception();
    }

    if (ex) common::rethrow_and_clear(ex);
}

}  // namespace dftracer::utils::utilities::fileio::lines::sources

#endif
