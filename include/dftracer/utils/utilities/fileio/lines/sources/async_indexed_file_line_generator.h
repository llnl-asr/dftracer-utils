#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_INDEXED_FILE_LINE_GENERATOR_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_INDEXED_FILE_LINE_GENERATOR_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>
#include <dftracer/utils/utilities/fileio/lines/sources/indexed_file_line_iterator.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/stream.h>
#include <dftracer/utils/utilities/reader/internal/stream_type.h>

#include <memory>
#include <string>

namespace dftracer::utils::utilities::fileio::lines::sources {

/**
 * @brief Async generator that yields lines from indexed archive files.
 *
 * Uses co_await on ReaderStream::read_async() for non-blocking I/O,
 * and co_yield to produce Line objects lazily.
 *
 * Usage:
 * @code
 * auto config = IndexedFileLineIteratorConfig()
 *     .with_file("file.gz", "file.gz.idx")
 *     .with_line_range(1, 100);
 *
 * auto gen = async_indexed_file_lines(config);
 * for (;;) {
 *     auto line = co_await gen.next();
 *     if (!line) break;
 *     process(*line);
 * }
 * @endcode
 */
inline coro::AsyncGenerator<Line> async_indexed_file_lines(
    IndexedFileLineIteratorConfig config) {
    if (!config.reader()) {
        throw DFTUtilsException(ErrorCode::InvalidArgument,
                                "Reader cannot be null");
    }

    // Resolve end if needed
    if (config.range_type() == reader::internal::RangeType::LineRange &&
        config.end() == 0) {
        std::size_t num_lines = config.reader()->get_num_lines();
        config = config.with_line_range(config.start(), num_lines);
    }

    // Build stream config
    reader::internal::StreamConfig stream_config;
    if (config.range_type() == reader::internal::RangeType::LineRange) {
        stream_config.stream_type(reader::internal::StreamType::Line)
            .range_type(reader::internal::RangeType::LineRange)
            .from(config.start())
            .to(config.end());
    } else {
        stream_config.stream_type(reader::internal::StreamType::LineBytes)
            .range_type(reader::internal::RangeType::ByteRange)
            .from(config.start())
            .to(config.end());
    }

    auto stream = config.reader()->stream(stream_config);
    if (!stream) {
        throw DFTUtilsException(ErrorCode::Io, "Failed to create stream");
    }

    std::string stream_buffer;
    stream_buffer.resize(config.buffer_size());
    std::string line_buffer;
    std::size_t current_position = config.start();
    const bool is_line_range =
        config.range_type() == reader::internal::RangeType::LineRange;

    while (!stream->done() &&
           (!is_line_range || current_position <= config.end())) {
        // Async read - this is the key difference from sync version
        std::size_t bytes_read = co_await stream->read_async(
            stream_buffer.data(), stream_buffer.size());

        if (bytes_read == 0) break;

        // Strip trailing newline
        if (bytes_read > 0 && stream_buffer[bytes_read - 1] == '\n') {
            bytes_read--;
        }

        line_buffer.assign(stream_buffer.data(), bytes_read);
        co_yield Line(std::string_view(line_buffer), current_position);
        current_position++;
    }
}

}  // namespace dftracer::utils::utilities::fileio::lines::sources

#endif
