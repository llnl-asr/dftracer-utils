#ifndef DFTRACER_UTILS_CORE_COMMON_HASH_SPLITMIX64_H
#define DFTRACER_UTILS_CORE_COMMON_HASH_SPLITMIX64_H

#include <dftracer/utils/core/common/hash/constants.h>

#include <cstdint>

namespace dftracer::utils::hash {

/// SplitMix64 finalizer: a fast, well-avalanched hash of a 64-bit value.
///
/// The finalizer from Sebastiano Vigna's SplitMix64 generator (public domain /
/// CC0): https://prng.di.unimi.it/splitmix64.c. The plugin ABI keeps its own
/// self-contained copy as dftu_mix64 (plugins/prims.h); the constants match, so
/// the two agree bit-for-bit.
inline std::uint64_t splitmix64(std::uint64_t x) {
    x += GOLDEN_RATIO;
    x = (x ^ (x >> 30)) * SPLITMIX64_MUL1;
    x = (x ^ (x >> 27)) * SPLITMIX64_MUL2;
    return x ^ (x >> 31);
}

}  // namespace dftracer::utils::hash

#endif  // DFTRACER_UTILS_CORE_COMMON_HASH_SPLITMIX64_H
