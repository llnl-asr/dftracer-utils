#ifndef DFTRACER_UTILS_JSON_LINE_H
#define DFTRACER_UTILS_JSON_LINE_H

#include <cstddef>

namespace dftracer::utils::json {
bool trim_and_validate(const char* data, std::size_t length, const char*& start,
                       std::size_t& trimmed_length);

// Trim whitespace and trailing commas (for JSON array format)
bool trim_and_validate_with_comma(const char* data, std::size_t length,
                                  const char*& start,
                                  std::size_t& trimmed_length);
}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_LINE_H
