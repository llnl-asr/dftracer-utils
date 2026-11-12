// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_HASH_XXH3_H
#define DFTRACER_UTILS_DSTD_HASH_XXH3_H

#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>

// Every xxHash symbol becomes static inline, so none clashes with the copy
// inside zstd and none is exported.
#define XXH_INLINE_ALL
#include <dftracer/utils/dstd/vendor/xxhash.h>
#undef XXH_INLINE_ALL

namespace dftracer::utils::dstd::hash {

/// XXH3 64-bit, seedless, of @p size bytes. The output is fixed by the XXH3
/// specification across versions, platforms and vector paths, so it is safe
/// to store. It avalanches.
inline std::uint64_t xxh3_64(const void* data, std::size_t size) noexcept {
    return static_cast<std::uint64_t>(XXH3_64bits(data, size));
}

inline std::uint64_t xxh3_64(StringView text) noexcept {
    return xxh3_64(text.data(), text.size());
}

}  // namespace dftracer::utils::dstd::hash

#endif  // DFTRACER_UTILS_DSTD_HASH_XXH3_H
