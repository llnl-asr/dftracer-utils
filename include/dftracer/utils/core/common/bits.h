#ifndef DFTRACER_UTILS_CORE_COMMON_BITS_H
#define DFTRACER_UTILS_CORE_COMMON_BITS_H

#include <cstddef>
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

/// `x` rounded up to a multiple of `a`; `x` itself when `a` is 0.
inline std::size_t align_up(std::size_t x, std::size_t a) {
    return a ? (x + a - 1) / a * a : x;
}

}  // namespace dftracer::utils::bits

#endif  // DFTRACER_UTILS_CORE_COMMON_BITS_H
