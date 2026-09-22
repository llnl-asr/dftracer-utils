#ifndef DFTRACER_UTILS_CORE_COMMON_BITS_H
#define DFTRACER_UTILS_CORE_COMMON_BITS_H

#include <cstdint>

namespace dftracer::utils::bits {

/// Count leading zero bits of `x`; 64 when `x` is 0.
inline int clz_u64(std::uint64_t x) { return x == 0 ? 64 : __builtin_clzll(x); }

/// Count trailing zero bits of `x`; 64 when `x` is 0.
inline int ctz_u64(std::uint64_t x) { return x == 0 ? 64 : __builtin_ctzll(x); }

/// Number of set bits in `x`.
inline int popcount_u64(std::uint64_t x) { return __builtin_popcountll(x); }

/// Floor of log2(`x`): index of the highest set bit; 0 when `x` is 0.
inline int ilog2_u64(std::uint64_t x) {
    return x == 0 ? 0 : 63 - __builtin_clzll(x);
}

/// Number of bits needed to represent `x` (ilog2 + 1); 0 when `x` is 0.
inline int bit_width_u64(std::uint64_t x) {
    return x == 0 ? 0 : 64 - __builtin_clzll(x);
}

}  // namespace dftracer::utils::bits

#endif  // DFTRACER_UTILS_CORE_COMMON_BITS_H
