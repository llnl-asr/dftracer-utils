// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_INDEXED_FILE_BYTES_GENERATOR_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_INDEXED_FILE_BYTES_GENERATOR_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/stream.h>
#include <dftracer/utils/utilities/reader/internal/stream_type.h>

namespace dftracer::utils::utilities::fileio::lines::sources {

/**
 * @brief Async generator that yields lines within byte boundaries
 *        from indexed archive files.
 *
 * Usage:
 * @code
 * auto reader = ok(ReaderFactory::create("file.gz", "file.gz.idx"));
 * auto gen = async_indexed_file_bytes(reader, 1000, 5000);
 * for (;;) {
 *     auto line = co_await gen.next();
 *     if (!line) break;
 *     if (!*line) return handle(line->error());
 *     process(**line);
 * }
 * @endcode
 *
 * A null reader, an empty byte range or a failed read is yielded as an error
 * item, after which the generator ends.
 */
inline coro::AsyncGenerator<dstd::Result<Line>> async_indexed_file_bytes(
    dstd::SharedPtr<reader::internal::Reader> reader, std::size_t start_byte,
    std::size_t end_byte, std::size_t buffer_size = 1024 * 1024) {
    if (!reader) {
        co_yield dstd::Result<Line>(common::make_error(
            common::ErrorCode::InvalidArgument, "Reader cannot be null"));
        co_return;
    }
    if (start_byte >= end_byte) {
        co_yield dstd::Result<Line>(common::make_error(
            common::ErrorCode::InvalidArgument, "Invalid byte range"));
        co_return;
    }

    auto opened =
        reader->stream(reader::internal::StreamConfig()
                           .stream_type(reader::internal::StreamType::LineBytes)
                           .range_type(reader::internal::RangeType::ByteRange)
                           .from(start_byte)
                           .to(end_byte));

    if (!opened.has_value()) {
        co_yield dstd::Result<Line>(opened.error());
        co_return;
    }
    auto stream = std::move(*opened);

    dstd::String stream_buffer;
    stream_buffer.resize(buffer_size);
    dstd::String line_buffer;
    std::size_t current_line = 1;

    while (!stream->done()) {
        auto read = co_await stream->read_async(stream_buffer.data(),
                                                stream_buffer.size());
        if (!read.has_value()) {
            co_yield dstd::Result<Line>(read.error());
            co_return;
        }
        std::size_t bytes_read = *read;

        if (bytes_read == 0) break;

        if (bytes_read > 0 && stream_buffer[bytes_read - 1] == '\n') {
            bytes_read--;
        }

        line_buffer.assign(stream_buffer.data(), bytes_read);
        co_yield Line(dstd::StringView(line_buffer), current_line);
        current_line++;
    }
}

}  // namespace dftracer::utils::utilities::fileio::lines::sources

#endif
