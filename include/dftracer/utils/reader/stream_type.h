#ifndef DFTRACER_UTILS_READER_STREAM_TYPE_H
#define DFTRACER_UTILS_READER_STREAM_TYPE_H

#ifdef __cplusplus
namespace dftracer::utils {

/**
 * @brief Type of stream to create.
 */
enum class StreamType {
    BYTES,              ///< Raw bytes, no line awareness
    LINE_BYTES,         ///< Line-boundary-aligned bytes (one line at a time)
    MULTI_LINES_BYTES,  ///< Line-boundary-aligned bytes (multiple lines)
    LINE,               ///< Single parsed line per read()
    MULTI_LINES         ///< Multiple parsed lines per read()
};

/**
 * @brief How to interpret start/end range parameters.
 */
enum class RangeType {
    BYTE_RANGE,  ///< start/end are byte offsets
    LINE_RANGE   ///< start/end are line numbers (1-based)
};

}  // namespace dftracer::utils

extern "C" {
#endif

/**
 * @brief Type of stream (C API).
 */
typedef enum {
    DFT_STREAM_TYPE_BYTES = 0, /** Raw bytes, no line awareness */
    DFT_STREAM_TYPE_LINE_BYTES =
        1, /** Line-boundary-aligned bytes (one line at a time) */
    DFT_STREAM_TYPE_MULTI_LINES_BYTES =
        2, /** Line-boundary-aligned bytes (multiple lines) */
    DFT_STREAM_TYPE_LINE = 3,       /** Single parsed line per read() */
    DFT_STREAM_TYPE_MULTI_LINES = 4 /** Multiple parsed lines per read() */
} dft_stream_type_t;

/**
 * @brief Range type (C API).
 */
typedef enum {
    DFT_RANGE_TYPE_BYTES = 0, /** start/end are byte offsets */
    DFT_RANGE_TYPE_LINES = 1  /** start/end are line numbers (1-based) */
} dft_range_type_t;

#ifdef __cplusplus
}
#endif

#endif  // DFTRACER_UTILS_READER_STREAM_TYPE_H
