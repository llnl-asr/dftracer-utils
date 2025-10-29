#ifndef DFTRACER_UTILS_UTILITIES_IO_LINES_LINES_H
#define DFTRACER_UTILS_UTILITIES_IO_LINES_LINES_H

/**
 * @file lines.h
 * @brief Convenience header that includes all line-related I/O utilities.
 *
 * This header provides access to:
 * - Line types (Line, Lines, etc.)
 * - Line range iterators (LineRange, LineBytesRange)
 * - Streaming line reader utility
 * - All line source iterators
 *
 * Usage:
 * @code
 * #include <dftracer/utils/utilities/io/lines/lines.h>
 *
 * // Read lines from a file
 * auto range = StreamingLineReader::read("data.txt");
 * while (range.has_next()) {
 *     Line line = range.next();
 *     // Process line...
 * }
 * @endcode
 */

#include <dftracer/utils/utilities/io/lines/line_bytes_range.h>
#include <dftracer/utils/utilities/io/lines/line_range.h>
#include <dftracer/utils/utilities/io/lines/line_types.h>
#include <dftracer/utils/utilities/io/lines/sources/sources.h>

#endif  // DFTRACER_UTILS_UTILITIES_IO_LINES_LINES_H
