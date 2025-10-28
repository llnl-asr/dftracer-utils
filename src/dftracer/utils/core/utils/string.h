#ifndef DFTRACER_UTILS_CORE_UTILS_STRING_H
#define DFTRACER_UTILS_CORE_UTILS_STRING_H

#include <cstddef>

namespace dftracer::utils {
bool json_trim_and_validate(const char* data, std::size_t length,
                            const char*& start, std::size_t& trimmed_length);

// Trim whitespace and trailing commas (for JSON array format)
bool json_trim_and_validate_with_comma(const char* data, std::size_t length,
                                       const char*& start,
                                       std::size_t& trimmed_length);
}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_UTILS_STRING_H
