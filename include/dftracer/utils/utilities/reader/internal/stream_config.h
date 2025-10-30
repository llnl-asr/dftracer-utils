#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAM_CONFIG_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAM_CONFIG_H

#include <dftracer/utils/utilities/reader/internal/stream_type.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Stream configuration (C API).
 *
 * Configuration for creating streams with control over stream type,
 * range, and internal buffer size.
 *
 * Example (C):
 * @code
 * dft_stream_config_t config = {
 *     .stream_type = DFT_STREAM_TYPE_LINE,
 *     .range_type = DFT_RANGE_TYPE_LINES,
 *     .start = 1,
 *     .end = 1000,
 *     .buffer_size = 512 * 1024 * 1024  // 512MB for large files
 * };
 * dft_reader_stream_t stream = dft_reader_stream(reader, &config);
 * @endcode
 */
typedef struct {
    /** Type of stream (BYTES, LINE, MULTI_LINES, etc.) */
    dft_stream_type_t stream_type;

    /** How to interpret start/end (BYTES or LINES) */
    dft_range_type_t range_type;

    /** Start of range (byte offset or line number based on range_type) */
    size_t start;

    /** End of range (byte offset or line number based on range_type) */
    size_t end;

    /**
     * Internal buffer size in bytes (0 = use default).
     *
     * Buffer size guidelines:
     * - Small files (<100MB): 1-4 MB
     * - Medium files (100MB-10GB): 16-64 MB (default)
     * - Large files (10GB-1TB): 128-512 MB
     *
     * Larger buffers improve I/O performance but use more memory.
     */
    size_t buffer_size;
} dft_stream_config_t;

#ifdef __cplusplus
}  // extern "C"

#include <cstddef>

namespace dftracer::utils::utilities::reader::internal {

/**
 * @brief Stream configuration (C++ API).
 *
 * Plain struct containing stream parameters. Can be constructed directly
 * or using StreamConfigManager for fluent API.
 *
 * Example (C++):
 * @code
 * // Fluent API (recommended)
 * auto stream = reader->stream(
 *     StreamConfig()
 *         .lines(1, 1000)
 *         .huge_buffer()  // 512MB
 * );
 *
 * // Or chain basic methods
 * auto stream = reader->stream(
 *     StreamConfig()
 *         .type(StreamType::LINE)
 *         .range(RangeType::LINE_RANGE)
 *         .from(1)
 *         .to(1000)
 *         .buffer(512 * 1024 * 1024)
 * );
 *
 * // Or direct initialization (C++20)
 * StreamConfig config{
 *     .stream_type = StreamType::LINE,
 *     .range_type = RangeType::LINE_RANGE,
 *     .start = 1,
 *     .end = 1000,
 *     .buffer_size = 512 * 1024 * 1024
 * };
 * auto stream = reader->stream(config);
 * @endcode
 */
class StreamConfig {
   public:
    StreamConfig() = default;
    StreamConfig(StreamType stream_type, RangeType range_type,
                 std::size_t start, std::size_t end, std::size_t buffer_size)
        : stream_type_(stream_type),
          range_type_(range_type),
          start_(start),
          end_(end),
          buffer_size_(buffer_size) {}

    static constexpr std::size_t DEFAULT_BUFFER_SIZE = 4 * 1024 * 1024;  // 4MB
    // ========================================================================
    // Fluent API - Basic Setters
    // ========================================================================

    /**
     * @brief Get stream type.
     */
    StreamType stream_type() const { return stream_type_; }

    /**
     * @brief Set stream type.
     */
    StreamConfig& stream_type(StreamType t) {
        stream_type_ = t;
        return *this;
    }

    /**
     * @brief Get range type.
     */
    RangeType range_type() const { return range_type_; }

    /**
     * @brief Set range type.
     */
    StreamConfig& range_type(RangeType t) {
        range_type_ = t;
        return *this;
    }

    /**
     * @brief Get start position.
     */
    std::size_t start() const { return start_; }

    /**
     * @brief Set start position.
     */
    StreamConfig& from(std::size_t s) {
        start_ = s;
        return *this;
    }

    /**
     * @brief Get end position.
     */
    std::size_t end() const { return end_; }

    /**
     * @brief Set end position.
     */
    StreamConfig& to(std::size_t e) {
        end_ = e;
        return *this;
    }

    /**
     * @brief Get buffer size in bytes.
     */
    std::size_t buffer_size() const { return buffer_size_; }

    /**
     * @brief Set buffer size in bytes.
     */
    StreamConfig& buffer_size(std::size_t size = 0) {
        if (size == 0) {
            // Reset to default 4MB
            buffer_size_ = DEFAULT_BUFFER_SIZE;
            return *this;
        }
        buffer_size_ = size;
        return *this;
    }

    // ========================================================================
    // C API Conversion
    // ========================================================================

    /**
     * @brief Convert to C API config.
     */
    dft_stream_config_t to_c() const {
        return dft_stream_config_t{static_cast<dft_stream_type_t>(stream_type_),
                                   static_cast<dft_range_type_t>(range_type_),
                                   start_, end_, buffer_size_};
    }

    /**
     * @brief Create from C API config.
     */
    static StreamConfig from_c(const dft_stream_config_t& c_config) {
        // Use default buffer size if 0 or uninitialized
        std::size_t buffer_size = c_config.buffer_size == 0
                                      ? DEFAULT_BUFFER_SIZE
                                      : c_config.buffer_size;
        return StreamConfig{static_cast<StreamType>(c_config.stream_type),
                            static_cast<RangeType>(c_config.range_type),
                            c_config.start, c_config.end, buffer_size};
    }

   private:
    /** Type of stream (BYTES, LINE, MULTI_LINES, etc.) */
    StreamType stream_type_ = StreamType::LINE;

    /** How to interpret start/end (BYTE_RANGE or LINE_RANGE) */
    RangeType range_type_ = RangeType::BYTE_RANGE;

    /** Start of range (byte offset or line number based on range_type) */
    std::size_t start_ = 0;

    /** End of range (byte offset or line number based on range_type) */
    std::size_t end_ = 0;

    /**
     * Internal buffer size in bytes (0 = use default).
     *
     * Larger buffers improve I/O performance but use more memory.
     */
    std::size_t buffer_size_ = 4 * 1024 * 1024;  // 4MB default
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // __cplusplus

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAM_CONFIG_H
