// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_FILEIO_LINES_LINE_TYPES_H
#define DFTRACER_UTILS_FILEIO_LINES_LINE_TYPES_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace dftracer::utils::fileio::lines {

/**
 * @brief Represents a single line of text with metadata.
 *
 * This structure holds a line's content along with its line number,
 * enabling easy tracking and processing of lines from various sources.
 *
 * NOTE: Uses string_view for zero-copy performance. The view is only valid
 * until the next call to next() on the iterator.
 */
struct Line {
    dstd::StringView content;
    std::size_t line_number;  ///< 1-based line number

    Line() : line_number(0) {}
    Line(dstd::StringView text, std::size_t number)
        : content(text), line_number(number) {}

    bool empty() const { return content.empty(); }
    std::size_t size() const { return content.size(); }
};

}  // namespace dftracer::utils::fileio::lines

#endif  // DFTRACER_UTILS_FILEIO_LINES_LINE_TYPES_H
