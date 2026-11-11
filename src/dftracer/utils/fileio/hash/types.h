// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_FILEIO_HASH_TYPES_H
#define DFTRACER_UTILS_FILEIO_HASH_TYPES_H

#include <cstddef>

namespace dftracer::utils::fileio::hash {
/**
 * @brief Hash value for a string/line.
 */
struct Hash {
    std::size_t value = 0;

    Hash() = default;

    explicit Hash(std::size_t v) : value(v) {}

    /// Equality for caching support
    bool operator==(const Hash& other) const { return value == other.value; }

    bool operator!=(const Hash& other) const { return !(*this == other); }
};
}  // namespace dftracer::utils::fileio::hash

#endif  // DFTRACER_UTILS_FILEIO_HASH_TYPES_H
