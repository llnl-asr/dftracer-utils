// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_FLOAT_KEY_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_FLOAT_KEY_H

#include <cstdint>
#include <cstring>

namespace dftracer::utils::dataframe {

/// The bits that stand for a float as a group or distinct key, as pandas and
/// Polars key it: -0.0 is +0.0, and every NaN is one value whatever its sign
/// and payload. `bits` is the IEEE bit pattern of a Float16, Float32 or
/// Float64 (zero-extended), `width` its size in bytes (2, 4 or 8).
inline std::uint64_t canonical_float_bits(std::uint64_t bits,
                                          unsigned width) noexcept {
    const unsigned total = width * 8;
    const unsigned mantissa = width == 8 ? 52 : width == 4 ? 23 : 10;
    const std::uint64_t sign = std::uint64_t{1} << (total - 1);
    const std::uint64_t magnitude = bits & ~sign;
    const std::uint64_t inf = ((std::uint64_t{1} << (total - 1 - mantissa)) - 1)
                              << mantissa;
    if (magnitude == 0) return 0;
    if (magnitude > inf) return inf | (std::uint64_t{1} << (mantissa - 1));
    return bits;
}

/// The canonical bits of the float of `width` bytes at `p`.
inline std::uint64_t canonical_float_at(const void* p,
                                        unsigned width) noexcept {
    std::uint64_t bits = 0;
    std::memcpy(&bits, p, width);
    return canonical_float_bits(bits, width);
}

/// Rewrites the float of `width` bytes at `p` as its canonical bits.
inline void canonicalize_float(void* p, unsigned width) noexcept {
    const std::uint64_t bits = canonical_float_at(p, width);
    std::memcpy(p, &bits, width);
}

/// canonical_float_bits for a Float64.
inline std::uint64_t canonical_f64_bits(std::uint64_t bits) noexcept {
    return canonical_float_bits(bits, 8);
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_FLOAT_KEY_H
