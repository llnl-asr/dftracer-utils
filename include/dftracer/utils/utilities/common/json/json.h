#ifndef DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_H
#define DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_H

/**
 * @file json.h
 * @brief Common JSON utilities for the dftracer-utils library.
 *
 * Provides JsonValue - a lightweight zero-cost wrapper around yyjson_val*.
 */

#include <dftracer/utils/utilities/common/json/json_value.h>

#include <cstddef>

namespace dftracer::utils::utilities::common::json {

/// Stack buffer size for yyjson_alc_pool used in per-line JSON parsing.
/// 4KB is sufficient for typical trace events (few hundred bytes each).
/// If a line exceeds this, yyjson silently falls back to malloc.
inline constexpr std::size_t YYJSON_LINE_POOL_SIZE = 4096;

}  // namespace dftracer::utils::utilities::common::json

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_H
