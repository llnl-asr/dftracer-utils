#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_INDEXED_FILE_BYTES_GENERATOR_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_ASYNC_INDEXED_FILE_BYTES_GENERATOR_H

#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/stream.h>
#include <dftracer/utils/utilities/reader/internal/stream_type.h>

#include <memory>
#include <string>

namespace dftracer::utils::utilities::fileio::lines::sources {

/**
 * @brief Async generator that yields lines within byte boundaries
 *        from indexed archive files.
 *
 * Usage:
 * @code
 * auto reader = ReaderFactory::create("file.gz", "file.gz.idx");
 * auto gen = async_indexed_file_bytes(reader, 1000, 5000);
 * while (auto line = co_await gen.next()) {
 *     process(*line);
 * }
 * @endcode
 */
inline coro::AsyncGenerator<Line> async_indexed_file_bytes(
    std::shared_ptr<reader::internal::Reader> reader, std::size_t start_byte,
    std::size_t end_byte, std::size_t buffer_size = 1024 * 1024) {
    if (!reader) {
        throw std::invalid_argument("Reader cannot be null");
    }
    if (start_byte >= end_byte) {
        throw std::invalid_argument("Invalid byte range");
    }

    auto stream = reader->stream(
        reader::internal::StreamConfig()
            .stream_type(reader::internal::StreamType::LINE_BYTES)
            .range_type(reader::internal::RangeType::BYTE_RANGE)
            .from(start_byte)
            .to(end_byte));

    if (!stream) {
        throw std::runtime_error("Failed to create stream");
    }

    std::string stream_buffer;
    stream_buffer.resize(buffer_size);
    std::string line_buffer;
    std::size_t current_line = 1;

    while (!stream->done()) {
        std::size_t bytes_read = co_await stream->read_async(
            stream_buffer.data(), stream_buffer.size());

        if (bytes_read == 0) break;

        if (bytes_read > 0 && stream_buffer[bytes_read - 1] == '\n') {
            bytes_read--;
        }

        line_buffer.assign(stream_buffer.data(), bytes_read);
        co_yield Line(std::string_view(line_buffer), current_line);
        current_line++;
    }
}

}  // namespace dftracer::utils::utilities::fileio::lines::sources

#endif
