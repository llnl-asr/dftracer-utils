#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_LINES_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_LINES_H

/**
 * @file lines.h
 * @brief Convenience header that includes all line-related I/O utilities.
 *
 * This header provides access to:
 * - Line types (Line, Lines, etc.)
 * - Streaming line reader utility (async)
 * - The line source generators
 *
 * Usage:
 * @code
 * #include <dftracer/utils/utilities/fileio/lines/lines.h>
 *
 * auto gen = StreamingLineReader::read_async(config);
 * for (;;) {
 *     auto line = co_await gen.next();
 *     if (!line) break;
 *     // Process *line...
 * }
 * @endcode
 */

#include <dftracer/utils/utilities/fileio/lines/line_types.h>
#include <dftracer/utils/utilities/fileio/lines/sources/sources.h>

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_LINES_LINES_H
