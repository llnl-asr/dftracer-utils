// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_JSON_LINE_H
#define DFTRACER_UTILS_JSON_LINE_H

#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::json {
/// Sets @p trimmed to @p line without its surrounding whitespace, a view into
/// @p line. False for a line that is empty or only a bracket or brace.
bool trim_and_validate(dstd::StringView line, dstd::StringView& trimmed);

// Trim whitespace and trailing commas (for JSON array format)
bool trim_and_validate_with_comma(dstd::StringView line,
                                  dstd::StringView& trimmed);
}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_LINE_H
