// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_ERROR_H
#define DFTRACER_UTILS_DUQL_ERROR_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>

namespace dftracer::utils::duql {

/// Structured parse error with source location.
struct DuqlError {
    dstd::String message;    ///< Error description.
    std::size_t column = 0;  ///< Column in the line (0-indexed).
    dstd::String source;     ///< The source line holding the error.
    dstd::String indicator;  ///< Caret/tilde indicator line.
    std::size_t line = 1;    ///< Line of the error (1-indexed).

    /// Format as multi-line error message with source and indicator.
    dstd::String format() const;
};

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_ERROR_H
