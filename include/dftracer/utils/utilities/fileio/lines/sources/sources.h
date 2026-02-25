#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_SOURCES_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_SOURCES_H

/**
 * @file sources.h
 * @brief Convenience header that includes all line source iterators.
 *
 * This header provides access to all line iteration sources:
 * - Indexed file line iterators (for compressed files with index)
 * - Plain file line iterators (for regular text files)
 * - Indexed file bytes iterators (byte-range reading with index)
 * - Plain file bytes iterators (byte-range reading from plain files)
 * - Async indexed file bytes generators (async byte-range with index)
 * - Async indexed file line generators (for async reading with index)
 * - Async plain file bytes generators (async byte-range from plain files)
 * - Async plain file line generators (for async plain file reading)
 *
 * Usage:
 * @code
 * #include <dftracer/utils/utilities/fileio/lines/sources/sources.h>
 *
 * // All source iterators are now available
 * @endcode
 */

#include <dftracer/utils/utilities/fileio/lines/sources/async_indexed_file_bytes_generator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_indexed_file_line_generator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_plain_file_bytes_generator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_plain_file_line_generator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/indexed_file_bytes_iterator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/indexed_file_line_iterator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/plain_file_bytes_iterator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/plain_file_line_iterator.h>

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_SOURCES_H
