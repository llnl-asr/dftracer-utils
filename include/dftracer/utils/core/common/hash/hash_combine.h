#ifndef DFTRACER_UTILS_CORE_COMMON_HASH_COMBINE_H
#define DFTRACER_UTILS_CORE_COMMON_HASH_COMBINE_H

#include <dftracer/utils/core/common/hash/constants.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace dftracer::utils {

/// Alias for dftracer::utils::hash::GOLDEN_RATIO, kept for existing callers.
inline constexpr std::uint64_t HASH_GOLDEN_RATIO = hash::GOLDEN_RATIO;

/// Boost-style hash combine: fold value into seed.
inline void hash_combine(std::size_t& seed, std::size_t value) {
    seed ^= value + HASH_GOLDEN_RATIO + (seed << 6) + (seed >> 2);
}

/// Fold the std::hash of value into seed.
template <typename T>
inline void hash_combine_value(std::size_t& seed, const T& value) {
    hash_combine(seed, std::hash<T>{}(value));
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_HASH_COMBINE_H
