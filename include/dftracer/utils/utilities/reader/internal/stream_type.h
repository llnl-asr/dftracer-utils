#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAM_TYPE_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAM_TYPE_H

namespace dftracer::utils::utilities::reader::internal {

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

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAM_TYPE_H
