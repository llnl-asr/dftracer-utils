#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_READER_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_READER_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/reader/internal/line_processor.h>
#include <dftracer/utils/utilities/reader/internal/stream.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dftracer::utils::utilities::reader::internal {

/**
 * Abstract base interface for all reader implementations.
 * This provides a common API for GZIP, TAR.GZ, and other archive readers.
 */
class Reader {
   public:
    virtual ~Reader() = default;

    // Core metadata accessors
    virtual std::size_t get_max_bytes() const = 0;
    virtual std::size_t get_num_lines() const = 0;
    virtual const std::string &get_archive_path() const = 0;
    virtual const std::string &get_index_path() const = 0;
    virtual void set_buffer_size(std::size_t size) = 0;

    // Estimate line count for a byte range (for pre-allocation)
    virtual std::size_t estimate_lines_in_range(std::size_t start_bytes,
                                                std::size_t end_bytes) const {
        if (get_max_bytes() == 0) return 0;
        double lines_per_byte = static_cast<double>(get_num_lines()) /
                                static_cast<double>(get_max_bytes());
        double byte_range = static_cast<double>(end_bytes - start_bytes);
        return static_cast<std::size_t>(byte_range * lines_per_byte *
                                        1.1);  // 10% buffer
    }

    // Async reading operations (coroutine)
    virtual coro::CoroTask<std::size_t> read_async(std::size_t start_bytes,
                                                   std::size_t end_bytes,
                                                   char *buffer,
                                                   std::size_t buffer_size) = 0;
    virtual coro::CoroTask<std::size_t> read_line_bytes_async(
        std::size_t start_bytes, std::size_t end_bytes, char *buffer,
        std::size_t buffer_size) = 0;
    virtual coro::CoroTask<std::string> read_lines_async(
        std::size_t start_line, std::size_t end_line) = 0;
    virtual coro::CoroTask<void> read_lines_with_processor_async(
        std::size_t start_line, std::size_t end_line,
        LineProcessor &processor) = 0;
    virtual coro::CoroTask<void> read_line_bytes_with_processor_async(
        std::size_t start_bytes, std::size_t end_bytes,
        LineProcessor &processor) = 0;

    // Synchronous reading operations (old API)
    std::size_t read(std::size_t start_bytes, std::size_t end_bytes,
                     char *buffer, std::size_t buffer_size) {
        return read_async(start_bytes, end_bytes, buffer, buffer_size).get();
    }
    std::size_t read_line_bytes(std::size_t start_bytes, std::size_t end_bytes,
                                char *buffer, std::size_t buffer_size) {
        return read_line_bytes_async(start_bytes, end_bytes, buffer,
                                     buffer_size)
            .get();
    }
    std::string read_lines(std::size_t start_line, std::size_t end_line) {
        return read_lines_async(start_line, end_line).get();
    }
    // Stream creation
    /**
     * @brief Create a stream for incremental reading.
     *
     * Supports all combinations of stream type and range type:
     * - BYTES + BYTE_RANGE: Raw bytes in exact byte range
     * - BYTES + LINE_RANGE: Raw bytes covering the line range
     * - LINE_BYTES + BYTE_RANGE: Single line-aligned bytes per read
     * - LINE_BYTES + LINE_RANGE: Single line-aligned bytes per read in line
     * range
     * - MULTI_LINES_BYTES + BYTE_RANGE: Multiple line-aligned bytes per read
     * - MULTI_LINES_BYTES + LINE_RANGE: Multiple line-aligned bytes per read in
     * line range
     * - LINE + BYTE_RANGE: Single parsed line per read in byte range
     * - LINE + LINE_RANGE: Single parsed line per read in line range
     * - MULTI_LINES + BYTE_RANGE: Multiple parsed lines per read in byte range
     * - MULTI_LINES + LINE_RANGE: Multiple parsed lines per read in line range
     *
     * Example:
     * @code
     * auto stream = reader->stream(
     *     StreamConfig()
     *         .lines(1, 1000000)
     *         .huge_buffer()  // 512MB
     * );
     *
     * // Zero-copy reading
     * while (!stream->done()) {
     *     auto chunk = stream->read();
     *     if (chunk.empty()) break;
     *     process(chunk);
     * }
     * @endcode
     *
     * @param config Stream configuration (use StreamConfig fluent API)
     * @return Unique pointer to stream, or nullptr on error
     */
    virtual std::unique_ptr<ReaderStream> stream(
        const StreamConfig &config) = 0;

    // State management
    virtual void reset() = 0;
    virtual bool is_valid() const = 0;

    // Format identification
    virtual std::string get_format_name() const = 0;

   protected:
    Reader() = default;
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_READER_H
