#ifndef DFTRACER_UTILS_DSTD_HASH_COMBINE_H
#define DFTRACER_UTILS_DSTD_HASH_COMBINE_H

#include <dftracer/utils/dstd/hash/constants.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace dftracer::utils::dstd {

/// Alias for hash::GOLDEN_RATIO, kept for existing callers.
inline constexpr std::uint64_t HASH_GOLDEN_RATIO = hash::GOLDEN_RATIO;

/// Fold value into seed. The formula is boost::hash_combine from Boost 1.33
/// (Daniel James), Boost Software License 1.0:
/// https://www.boost.org/doc/libs/1_33_1/doc/html/hash_combine.html
inline void hash_combine(std::size_t& seed, std::size_t value) {
    seed ^= value + HASH_GOLDEN_RATIO + (seed << 6) + (seed >> 2);
}

/// Fold the std::hash of value into seed.
template <typename T>
inline void hash_combine_value(std::size_t& seed, const T& value) {
    hash_combine(seed, std::hash<T>{}(value));
}

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_HASH_COMBINE_H
