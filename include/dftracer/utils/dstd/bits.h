#ifndef DFTRACER_UTILS_DSTD_BITS_H
#define DFTRACER_UTILS_DSTD_BITS_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

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

/// `x` rounded up to a multiple of `a`, or `x` itself when `a` is 0. The result
/// has the common type of the arguments.
template <typename T, typename U>
constexpr std::common_type_t<T, U> align_up(T x, U a) {
    using R = std::common_type_t<T, U>;
    return a ? (static_cast<R>(x) + static_cast<R>(a) - 1) / static_cast<R>(a) *
                   static_cast<R>(a)
             : static_cast<R>(x);
}

/// `n / d` rounded up, for a positive `d` and a `n` that is not negative. The
/// result has the common type of the arguments.
template <typename T, typename U>
constexpr std::common_type_t<T, U> div_ceil(T n, U d) {
    using R = std::common_type_t<T, U>;
    return (static_cast<R>(n) + static_cast<R>(d) - 1) / static_cast<R>(d);
}

}  // namespace dftracer::utils::bits

#endif  // DFTRACER_UTILS_DSTD_BITS_H
