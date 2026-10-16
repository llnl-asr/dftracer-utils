#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAM_TYPE_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAM_TYPE_H

namespace dftracer::utils::utilities::reader::internal {

/**
 * @brief Type of stream to create.
 */
enum class StreamType {
    Bytes,            ///< Raw bytes, no line awareness
    LineBytes,        ///< Line-boundary-aligned bytes (one line at a time)
    MultiLinesBytes,  ///< Line-boundary-aligned bytes (multiple lines)
    Line,             ///< Single parsed line per read()
    MultiLines        ///< Multiple parsed lines per read()
};

/**
 * @brief How to interpret start/end range parameters.
 */
enum class RangeType {
    ByteRange,  ///< start/end are byte offsets
    LineRange   ///< start/end are line numbers (1-based)
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAM_TYPE_H
