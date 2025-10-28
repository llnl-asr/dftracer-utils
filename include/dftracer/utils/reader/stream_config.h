#ifndef DFTRACER_UTILS_READER_STREAM_CONFIG_H
#define DFTRACER_UTILS_READER_STREAM_CONFIG_H

#include <dftracer/utils/reader/stream_type.h>
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
    dft_stream_type_t stream;

    /** How to interpret start/end (BYTES or LINES) */
    dft_range_type_t range;

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

/**
 * @brief Initialize stream config with defaults.
 *
 * Creates a config for line-based streaming with default buffer size.
 *
 * @param config Pointer to config to initialize
 * @param start Start line number
 * @param end End line number
 */
static inline void dft_stream_config_init_lines(dft_stream_config_t* config,
                                                size_t start, size_t end) {
    config->stream = DFT_STREAM_TYPE_LINE;
    config->range = DFT_RANGE_TYPE_LINES;
    config->start = start;
    config->end = end;
    config->buffer_size = 0;  // Default
}

/**
 * @brief Initialize stream config for byte reading.
 *
 * @param config Pointer to config to initialize
 * @param start Start byte offset
 * @param end End byte offset
 */
static inline void dft_stream_config_init_bytes(dft_stream_config_t* config,
                                                size_t start, size_t end) {
    config->stream = DFT_STREAM_TYPE_BYTES;
    config->range = DFT_RANGE_TYPE_BYTES;
    config->start = start;
    config->end = end;
    config->buffer_size = 0;  // Default
}

/**
 * @brief Set buffer size on config.
 *
 * @param config Pointer to config
 * @param buffer_size Buffer size in bytes (0 = default)
 */
static inline void dft_stream_config_set_buffer(dft_stream_config_t* config,
                                                size_t buffer_size) {
    config->buffer_size = buffer_size;
}

#ifdef __cplusplus
}  // extern "C"

#include <cstddef>

namespace dftracer::utils {

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
struct StreamConfig {
    /** Type of stream (BYTES, LINE, MULTI_LINES, etc.) */
    StreamType stream = StreamType::LINE;

    /** How to interpret start/end (BYTE_RANGE or LINE_RANGE) */
    RangeType range = RangeType::BYTE_RANGE;

    /** Start of range (byte offset or line number based on range_type) */
    std::size_t start = 0;

    /** End of range (byte offset or line number based on range_type) */
    std::size_t end = 0;

    /**
     * Internal buffer size in bytes (0 = use default).
     *
     * Larger buffers improve I/O performance but use more memory.
     */
    std::size_t buffer_size = 4 * 1024 * 1024;  // 4MB default

    // ========================================================================
    // Fluent API - Basic Setters
    // ========================================================================

    /**
     * @brief Set stream type.
     */
    StreamConfig& stream_type(StreamType t) {
        stream = t;
        return *this;
    }

    /**
     * @brief Set range type.
     */
    StreamConfig& range_type(RangeType t) {
        range = t;
        return *this;
    }

    /**
     * @brief Set start position.
     */
    StreamConfig& from(std::size_t s) {
        start = s;
        return *this;
    }

    /**
     * @brief Set end position.
     */
    StreamConfig& to(std::size_t e) {
        end = e;
        return *this;
    }

    /**
     * @brief Set buffer size in bytes.
     */
    StreamConfig& buffer(std::size_t size) {
        if (size == 0) {
            // Reset to default 4MB
            buffer_size = 4 * 1024 * 1024;
            return *this;
        }
        buffer_size = size;
        return *this;
    }

    /*
     * @brief Set range start and end.
     */
    StreamConfig& from_to(std::size_t s, std::size_t e) {
        start = s;
        end = e;
        return *this;
    }

    // ========================================================================
    // C API Conversion
    // ========================================================================

    /**
     * @brief Convert to C API config.
     */
    dft_stream_config_t to_c() const {
        return dft_stream_config_t{static_cast<dft_stream_type_t>(stream),
                                   static_cast<dft_range_type_t>(range), start,
                                   end, buffer_size};
    }

    /**
     * @brief Create from C API config.
     */
    static StreamConfig from_c(const dft_stream_config_t& c_config) {
        return StreamConfig{static_cast<StreamType>(c_config.stream),
                            static_cast<RangeType>(c_config.range),
                            c_config.start, c_config.end, c_config.buffer_size};
    }
};

}  // namespace dftracer::utils

#endif  // __cplusplus

#endif  // DFTRACER_UTILS_READER_STREAM_CONFIG_H
