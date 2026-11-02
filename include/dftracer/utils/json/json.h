// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_JSON_JSON_H
#define DFTRACER_UTILS_JSON_JSON_H

/**
 * @file json.h
 * @brief Common JSON utilities for the dftracer-utils library.
 *
 * Provides JsonValue - a lightweight zero-cost wrapper around simdjson DOM
 * elements.
 */

#include <dftracer/utils/json/json_value.h>

#include <cstddef>

namespace dftracer::utils::json {

/// Default capacity for simdjson parser buffer.
/// 1MB is sufficient for most JSON documents.
inline constexpr std::size_t SIMDJSON_DEFAULT_CAPACITY = 1 << 20;

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_JSON_H
