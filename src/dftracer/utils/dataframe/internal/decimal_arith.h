#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_DECIMAL_ARITH_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_DECIMAL_ARITH_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

namespace dftracer::utils::dataframe {

/// Max digits an exact-arithmetic result may declare, matching Arrow's
/// Decimal128 (fits exactly in a signed 128-bit integer: 10^38 < 2^127 - 1 <
/// 10^39) and Decimal256 limits.
constexpr std::int32_t DECIMAL128_MAX_PRECISION = 38;
constexpr std::int32_t DECIMAL256_MAX_PRECISION = 76;

/// SQL-style division scale extension (the convention several engines, e.g.
/// DB2, use for NUMERIC division): the quotient's scale is the dividend's
/// scale plus this fixed increment, rather than an unbounded exact fraction.
constexpr std::int32_t DECIMAL_DIVIDE_SCALE_INCREMENT = 4;

__extension__ typedef __int128 i128;
__extension__ typedef unsigned __int128 u128;

inline i128 load_i128(const void* bytes) noexcept {
    i128 v;
    std::memcpy(&v, bytes, sizeof(v));
    return v;
}
inline void store_i128(void* bytes, i128 v) noexcept {
    std::memcpy(bytes, &v, sizeof(v));
}

/// 10^n as an unsigned 128-bit value, for n in [0, 38]. Values above 38
/// would not fit int128 (see DECIMAL128_MAX_PRECISION) and are not needed.
inline u128 pow10_u128(std::int32_t n) noexcept {
    u128 v = 1;
    for (std::int32_t i = 0; i < n; ++i) v *= 10;
    return v;
}

/// True if |v| < 10^precision, i.e. `v` fits a decimal with that many digits.
/// precision <= 0 never fits (a decimal always has at least one digit).
inline bool i128_fits_precision(i128 v, std::int32_t precision) noexcept {
    if (precision <= 0) return false;
    if (precision >= DECIMAL128_MAX_PRECISION + 1)
        return true;  // no int128 value can exceed 10^39
    u128 mag = v < 0 ? static_cast<u128>(-(v + 1)) + 1 : static_cast<u128>(v);
    return mag < pow10_u128(precision);
}

/// Rescale `v` (currently at `from_scale`) up to `to_scale` (>= from_scale)
/// by multiplying by 10^(to_scale - from_scale). std::nullopt when the product
/// would not fit. The bound is checked BEFORE multiplying: signed overflow is
/// undefined, so a wrapped product cannot be tested after the fact.
inline std::optional<i128> rescale_up_i128(i128 v, std::int32_t from_scale,
                                           std::int32_t to_scale) noexcept {
    if (to_scale == from_scale) return v;
    if (v == 0) return i128{0};
    const u128 factor = pow10_u128(to_scale - from_scale);
    if (factor == 0) return std::nullopt;
    constexpr u128 I128_MAX = (static_cast<u128>(1) << 127) - 1;
    const bool neg = v < 0;
    const u128 mag = neg ? (~static_cast<u128>(v) + 1) : static_cast<u128>(v);
    if (mag > I128_MAX / factor) return std::nullopt;
    const i128 prod = static_cast<i128>(mag * factor);
    return neg ? -prod : prod;
}

/// Widening unsigned 128x128 -> 256-bit multiply, returned as {lo, hi}
/// (each 128 bits). Schoolbook multiplication via 64-bit halves so the
/// partial products fit unsigned __int128 without overflow.
struct U256 {
    u128 lo;
    u128 hi;
};

inline U256 umul128_256(u128 a, u128 b) noexcept {
    const std::uint64_t a_lo = static_cast<std::uint64_t>(a);
    const std::uint64_t a_hi = static_cast<std::uint64_t>(a >> 64);
    const std::uint64_t b_lo = static_cast<std::uint64_t>(b);
    const std::uint64_t b_hi = static_cast<std::uint64_t>(b >> 64);

    const u128 t0 = static_cast<u128>(a_lo) * b_lo;
    const u128 t1 = static_cast<u128>(a_lo) * b_hi;
    const u128 t2 = static_cast<u128>(a_hi) * b_lo;
    const u128 t3 = static_cast<u128>(a_hi) * b_hi;

    const std::uint64_t t0_lo = static_cast<std::uint64_t>(t0);
    const u128 mid = static_cast<u128>(static_cast<std::uint64_t>(t0 >> 64)) +
                     static_cast<std::uint64_t>(t1) +
                     static_cast<std::uint64_t>(t2);
    const std::uint64_t mid_lo = static_cast<std::uint64_t>(mid);
    const u128 carry = mid >> 64;

    const u128 hi = t3 + (t1 >> 64) + (t2 >> 64) + carry;
    const u128 lo = (static_cast<u128>(mid_lo) << 64) | t0_lo;
    return {lo, hi};
}

/// Signed 128x128 multiply widened to 256 bits, checked against a target
/// decimal precision. Returns std::nullopt if the exact product does not fit
/// signed int128 at all, or does not fit `result_precision` digits.
inline std::optional<i128> mul_checked_i128(
    i128 a, i128 b, std::int32_t result_precision) noexcept {
    const bool neg = (a < 0) != (b < 0);
    const u128 ua =
        a < 0 ? static_cast<u128>(-(a + 1)) + 1 : static_cast<u128>(a);
    const u128 ub =
        b < 0 ? static_cast<u128>(-(b + 1)) + 1 : static_cast<u128>(b);
    const U256 prod = umul128_256(ua, ub);
    if (prod.hi != 0) return std::nullopt;
    // Signed int128 magnitude ceiling is 2^127; the unsigned product can sit
    // right at that boundary only for the (impossible for a decimal-128
    // input) most-negative value, so a plain top-bit check is exact here.
    if (prod.lo >> 127 != 0) return std::nullopt;
    i128 result = static_cast<i128>(prod.lo);
    if (neg) result = -result;
    if (!i128_fits_precision(result, result_precision)) return std::nullopt;
    return result;
}

/// Exact `(a * 10^shift) / b`, truncating toward zero, checked against
/// `result_precision`. Used for decimal division: see decimal_divide_i128.
/// std::nullopt if the intermediate product does not fit 128 bits (the
/// scale shift is too large for the operand's magnitude), `b` is zero, or
/// the quotient does not fit `result_precision`.
inline std::optional<i128> muldiv_checked_i128(
    i128 a, std::int32_t shift, i128 b,
    std::int32_t result_precision) noexcept {
    if (b == 0 || shift < 0 || shift > DECIMAL128_MAX_PRECISION)
        return std::nullopt;
    const bool neg = (a < 0) != (b < 0);
    const u128 ua =
        a < 0 ? static_cast<u128>(-(a + 1)) + 1 : static_cast<u128>(a);
    const u128 ub =
        b < 0 ? static_cast<u128>(-(b + 1)) + 1 : static_cast<u128>(b);
    const U256 prod = umul128_256(ua, pow10_u128(shift));
    if (prod.hi != 0 || (prod.lo >> 127) != 0) return std::nullopt;
    const u128 quotient = prod.lo / ub;
    if (quotient >> 127 != 0) return std::nullopt;
    i128 result = static_cast<i128>(quotient);
    if (neg) result = -result;
    if (!i128_fits_precision(result, result_precision)) return std::nullopt;
    return result;
}

/// Decimal256 raw value as 4 little-endian 64-bit limbs (limb[0] least
/// significant), matching the on-disk/Arrow two's-complement byte layout.
using Limbs256 = std::array<std::uint64_t, 4>;

inline Limbs256 load_limbs256(const void* bytes) noexcept {
    Limbs256 v;
    std::memcpy(v.data(), bytes, sizeof(v));
    return v;
}
inline void store_limbs256(void* bytes, const Limbs256& v) noexcept {
    std::memcpy(bytes, v.data(), sizeof(v));
}
inline bool is_negative256(const Limbs256& v) noexcept {
    return (static_cast<std::int64_t>(v[3])) < 0;
}
inline Limbs256 negate256(Limbs256 v) noexcept {
    for (auto& w : v) w = ~w;
    u128 carry = 1;
    for (auto& w : v) {
        const u128 sum = static_cast<u128>(w) + carry;
        w = static_cast<std::uint64_t>(sum);
        carry = sum >> 64;
    }
    return v;
}
inline Limbs256 abs256(const Limbs256& v) noexcept {
    return is_negative256(v) ? negate256(v) : v;
}

/// Word-wise add of two 256-bit two's-complement values; the top carry out
/// of limb[3] is the signed-overflow indicator the caller checks.
inline Limbs256 add256_raw(const Limbs256& a, const Limbs256& b,
                           bool& carry_out) noexcept {
    Limbs256 out{};
    u128 carry = 0;
    for (int i = 0; i < 4; ++i) {
        const u128 sum = static_cast<u128>(a[i]) + b[i] + carry;
        out[i] = static_cast<std::uint64_t>(sum);
        carry = sum >> 64;
    }
    carry_out = carry != 0;
    return out;
}

/// Signed 256-bit add/sub overflow check: the classic two's-complement rule,
/// operands same sign but result differs.
inline bool add256_overflows(const Limbs256& a, const Limbs256& b,
                             const Limbs256& out) noexcept {
    const bool sa = is_negative256(a);
    const bool sb = is_negative256(b);
    const bool so = is_negative256(out);
    return sa == sb && so != sa;
}

/// `v * 10`, for building a 256-bit power-of-ten bound. `overflow` is set if
/// the product needed a 5th limb.
inline Limbs256 mul10_256(Limbs256 v, bool& overflow) noexcept {
    u128 carry = 0;
    for (auto& w : v) {
        const u128 prod = static_cast<u128>(w) * 10 + carry;
        w = static_cast<std::uint64_t>(prod);
        carry = prod >> 64;
    }
    overflow = carry != 0;
    return v;
}

/// 10^n as unsigned 256-bit limbs, for n in [0, 76] (DECIMAL256_MAX_PRECISION).
inline Limbs256 pow10_256(std::int32_t n) noexcept {
    Limbs256 v{1, 0, 0, 0};
    for (std::int32_t i = 0; i < n; ++i) {
        bool overflow = false;
        v = mul10_256(v, overflow);
    }
    return v;
}

/// Unsigned compare of two 256-bit magnitudes, most-significant limb first.
inline int compare_u256(const Limbs256& a, const Limbs256& b) noexcept {
    for (int i = 3; i >= 0; --i) {
        if (a[static_cast<std::size_t>(i)] != b[static_cast<std::size_t>(i)])
            return a[static_cast<std::size_t>(i)] <
                           b[static_cast<std::size_t>(i)]
                       ? -1
                       : 1;
    }
    return 0;
}

/// True if |v| < 10^precision.
inline bool limbs256_fits_precision(const Limbs256& v,
                                    std::int32_t precision) noexcept {
    if (precision <= 0) return false;
    if (precision > DECIMAL256_MAX_PRECISION) return true;
    return compare_u256(abs256(v), pow10_256(precision)) < 0;
}

/// `a + (neg ? -b : b)`, checked against `result_precision`. Used for both
/// Decimal256 add (neg=false) and subtract (neg=true).
inline std::optional<Limbs256> addsub_checked_256(
    const Limbs256& a, const Limbs256& b, bool neg,
    std::int32_t result_precision) noexcept {
    const Limbs256 rhs = neg ? negate256(b) : b;
    bool carry = false;
    const Limbs256 sum = add256_raw(a, rhs, carry);
    if (add256_overflows(a, rhs, sum)) return std::nullopt;
    if (!limbs256_fits_precision(sum, result_precision)) return std::nullopt;
    return sum;
}

/// Rescale a 256-bit value up from `from_scale` to `to_scale` (>= from_scale)
/// by multiplying by 10^(to_scale - from_scale), checked for overflow past
/// the 256-bit container.
inline std::optional<Limbs256> rescale_up_256(const Limbs256& v,
                                              std::int32_t from_scale,
                                              std::int32_t to_scale) noexcept {
    if (to_scale == from_scale) return v;
    const bool neg = is_negative256(v);
    Limbs256 mag = abs256(v);
    for (std::int32_t i = 0; i < to_scale - from_scale; ++i) {
        bool overflow = false;
        mag = mul10_256(mag, overflow);
        if (overflow || is_negative256(mag)) return std::nullopt;
    }
    return neg ? negate256(mag) : mag;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_DECIMAL_ARITH_H
