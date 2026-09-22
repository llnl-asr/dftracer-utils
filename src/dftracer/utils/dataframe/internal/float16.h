#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_FLOAT16_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_FLOAT16_H

#include <cstdint>
#include <cstring>

// IEEE 754 binary16 <-> binary32 conversion. Standard bit-manipulation
// technique (sign/exponent/mantissa reinterpretation with subnormal
// normalization and round-to-nearest-even on narrowing); not borrowed from a
// specific third-party source.
namespace dftracer::utils::dataframe {

/// Every half value has an exact float32 representation, so this never loses
/// precision.
inline float half_to_float(std::uint16_t h) {
    std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    std::uint32_t exp = (h & 0x7C00u) >> 10;
    std::uint32_t mant = h & 0x03FFu;
    std::uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            std::int32_t e = 1;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                --e;
            }
            mant &= 0x03FFu;
            bits = sign | (static_cast<std::uint32_t>(e + (127 - 15)) << 23) |
                   (mant << 13);
        }
    } else if (exp == 0x1Fu) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

/// Narrows a float32 to binary16 with round-to-nearest-even; out-of-range
/// magnitudes saturate to +-inf, matching IEEE 754 overflow behavior.
inline std::uint16_t float_to_half(float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    std::uint32_t sign = static_cast<std::uint32_t>((bits >> 16) & 0x8000u);
    std::uint32_t abs_exp = (bits >> 23) & 0xFFu;
    std::uint32_t mant = bits & 0x7FFFFFu;

    if (abs_exp == 0xFFu)
        return static_cast<std::uint16_t>(sign | 0x7C00u |
                                          (mant != 0 ? 0x200u : 0u));

    std::int32_t exp = static_cast<std::int32_t>(abs_exp) - 127 + 15;
    if (exp >= 0x1F) return static_cast<std::uint16_t>(sign | 0x7C00u);

    if (exp <= 0) {
        if (exp < -10) return static_cast<std::uint16_t>(sign);
        mant |= 0x800000u;
        std::uint32_t shift = static_cast<std::uint32_t>(14 - exp);
        std::uint32_t half_mant = mant >> shift;
        std::uint32_t remainder = mant & ((1u << shift) - 1u);
        std::uint32_t halfway = 1u << (shift - 1u);
        if (remainder > halfway || (remainder == halfway && (half_mant & 1u)))
            ++half_mant;
        return static_cast<std::uint16_t>(sign | half_mant);
    }

    std::uint32_t half_mant = mant >> 13;
    std::uint32_t remainder = mant & 0x1FFFu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half_mant & 1u))) {
        ++half_mant;
        if (half_mant == 0x400u) {
            half_mant = 0;
            ++exp;
            if (exp >= 0x1F) return static_cast<std::uint16_t>(sign | 0x7C00u);
        }
    }
    return static_cast<std::uint16_t>(
        sign | (static_cast<std::uint32_t>(exp) << 10) | half_mant);
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_FLOAT16_H
