#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_DECIMAL_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_DECIMAL_H

#include <cmath>
#include <cstdint>
#include <cstring>

namespace dftracer::utils::dataframe {

/// Decodes a little-endian two's-complement Decimal128 (16 bytes) at `scale`
/// into a double: exact for the integer value, then divided by 10^scale, so
/// this LOSES PRECISION relative to the source decimal - callers that need
/// exact decimal arithmetic must not go through this path (see
/// is_arithmetic_type's documentation).
inline double decimal128_to_double(const void* bytes, std::int32_t scale) {
    std::uint64_t low;
    std::int64_t high;
    std::memcpy(&low, bytes, sizeof(low));
    std::memcpy(&high, static_cast<const std::uint8_t*>(bytes) + 8,
                sizeof(high));
    // Two's-complement identity: value = high * 2^64 + low, exact regardless
    // of sign.
    double raw = static_cast<double>(high) * 18446744073709551616.0 +
                 static_cast<double>(low);
    return raw / std::pow(10.0, static_cast<double>(scale));
}

/// Decodes a little-endian two's-complement Decimal256 (32 bytes) at `scale`
/// into a double. Same precision caveat as decimal128_to_double.
inline double decimal256_to_double(const void* bytes, std::int32_t scale) {
    std::uint64_t w0, w1, w2;
    std::int64_t w3;
    const auto* p = static_cast<const std::uint8_t*>(bytes);
    std::memcpy(&w0, p, 8);
    std::memcpy(&w1, p + 8, 8);
    std::memcpy(&w2, p + 16, 8);
    std::memcpy(&w3, p + 24, 8);
    const double p64 = 18446744073709551616.0;
    double raw =
        ((static_cast<double>(w3) * p64 + static_cast<double>(w2)) * p64 +
         static_cast<double>(w1)) *
            p64 +
        static_cast<double>(w0);
    return raw / std::pow(10.0, static_cast<double>(scale));
}

/// Signed numeric ordering of two little-endian two's-complement Decimal128
/// values at `a`/`b` (16 bytes each), without going through a lossy double:
/// compares the signed high word first, then the unsigned low word.
inline int compare_decimal128(const void* a, const void* b) {
    std::int64_t ha, hb;
    std::memcpy(&ha, static_cast<const std::uint8_t*>(a) + 8, 8);
    std::memcpy(&hb, static_cast<const std::uint8_t*>(b) + 8, 8);
    if (ha != hb) return ha < hb ? -1 : 1;
    std::uint64_t la, lb;
    std::memcpy(&la, a, 8);
    std::memcpy(&lb, b, 8);
    if (la != lb) return la < lb ? -1 : 1;
    return 0;
}

/// Signed numeric ordering of two little-endian two's-complement Decimal256
/// values at `a`/`b` (32 bytes each): most-significant word first (signed),
/// then the remaining words (unsigned), most-significant first.
inline int compare_decimal256(const void* a, const void* b) {
    const auto* pa = static_cast<const std::uint8_t*>(a);
    const auto* pb = static_cast<const std::uint8_t*>(b);
    std::int64_t ha, hb;
    std::memcpy(&ha, pa + 24, 8);
    std::memcpy(&hb, pb + 24, 8);
    if (ha != hb) return ha < hb ? -1 : 1;
    for (int word = 2; word >= 0; --word) {
        std::uint64_t wa, wb;
        std::memcpy(&wa, pa + word * 8, 8);
        std::memcpy(&wb, pb + word * 8, 8);
        if (wa != wb) return wa < wb ? -1 : 1;
    }
    return 0;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_DECIMAL_H
