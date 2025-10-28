#ifndef DFTRACER_UTILS_READER_READER_H
#define DFTRACER_UTILS_READER_READER_H

#include <dftracer/utils/reader/line_processor.h>
#include <dftracer/utils/reader/stream.h>
#include <dftracer/utils/reader/stream_config.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declare indexer handle from indexer C API
typedef void *dft_indexer_handle_t;

/**
 * Opaque handle for DFT reader
 */
typedef void *dft_reader_handle_t;
dft_reader_handle_t dft_reader_create(const char *gz_path, const char *idx_path,
                                      size_t index_ckpt_size);
dft_reader_handle_t dft_reader_create_with_indexer(
    dft_indexer_handle_t indexer);
void dft_reader_destroy(dft_reader_handle_t reader);
int dft_reader_get_max_bytes(dft_reader_handle_t reader, size_t *max_bytes);
int dft_reader_get_num_lines(dft_reader_handle_t reader, size_t *num_lines);
int dft_reader_read(dft_reader_handle_t reader, size_t start_bytes,
                    size_t end_bytes, char *buffer, size_t buffer_size);
int dft_reader_read_line_bytes(dft_reader_handle_t reader, size_t start_bytes,
                               size_t end_bytes, char *buffer,
                               size_t buffer_size);
int dft_reader_read_lines(dft_reader_handle_t reader, size_t start_line,
                          size_t end_line, char *buffer, size_t buffer_size,
                          size_t *bytes_written);
int dft_reader_read_lines_with_processor(dft_reader_handle_t reader,
                                         size_t start_line, size_t end_line,
                                         dft_line_processor_callback_t callback,
                                         void *user_data);
void dft_reader_reset(dft_reader_handle_t reader);

/**
 * Create a stream for incremental reading with configuration.
 *
 * @param reader Reader handle
 * @param config Stream configuration (type, range, buffer size)
 * @return Stream handle, or NULL on error
 */
dft_reader_stream_t dft_reader_stream(dft_reader_handle_t reader,
                                      const dft_stream_config_t *config);

#ifdef __cplusplus
}  // extern "C"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dftracer::utils {

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
    virtual const std::string &get_idx_path() const = 0;
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

    // Raw byte reading operations
    virtual std::size_t read(std::size_t start_bytes, std::size_t end_bytes,
                             char *buffer, std::size_t buffer_size) = 0;
    virtual std::size_t read_line_bytes(std::size_t start_bytes,
                                        std::size_t end_bytes, char *buffer,
                                        std::size_t buffer_size) = 0;

    // Line-based reading operations
    virtual std::string read_lines(std::size_t start_line,
                                   std::size_t end_line) = 0;
    virtual void read_lines_with_processor(std::size_t start_line,
                                           std::size_t end_line,
                                           LineProcessor &processor) = 0;
    virtual void read_line_bytes_with_processor(std::size_t start_bytes,
                                                std::size_t end_bytes,
                                                LineProcessor &processor) = 0;

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
     *     auto chunk = stream->read();  // span_view - zero copy
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

}  // namespace dftracer::utils

#endif  // __cplusplus

#endif  // DFTRACER_UTILS_READER_READER_H
