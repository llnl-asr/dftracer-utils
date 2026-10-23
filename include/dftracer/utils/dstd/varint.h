#ifndef DFTRACER_UTILS_DSTD_VARINT_H
#define DFTRACER_UTILS_DSTD_VARINT_H

#include <cstddef>
#include <cstdint>

namespace dftracer::utils {

// LEB128 for u64: seven bits per byte, low group first, the high bit marks a
// following byte.

inline constexpr std::size_t VARINT_MAX_BYTES = 10;

/// Writes v at p and returns the end. The caller reserves VARINT_MAX_BYTES.
inline std::uint8_t* write_varint(std::uint8_t* p, std::uint64_t v) {
    while (v >= 0x80U) {
        *p++ = static_cast<std::uint8_t>(v | 0x80U);
        v >>= 7U;
    }
    *p++ = static_cast<std::uint8_t>(v);
    return p;
}

/// Appends to any byte vector (std::string, dstd::Vector<std::uint8_t>).
template <typename Buf>
inline void append_varint(Buf& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<typename Buf::value_type>(v | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<typename Buf::value_type>(v));
}

/// Decodes one varint from [p, end). On success advances p past it and returns
/// true. A truncated or over-long (more than 64 bits) value returns false and
/// leaves p and out unchanged.
inline bool read_varint(const std::uint8_t*& p, const std::uint8_t* end,
                        std::uint64_t& out) {
    std::uint64_t v = 0;
    unsigned shift = 0;
    const std::uint8_t* q = p;
    while (q != end) {
        const std::uint8_t b = *q++;
        if (shift == 63U && b > 1U) return false;
        v |= static_cast<std::uint64_t>(b & 0x7FU) << shift;
        if ((b & 0x80U) == 0) {
            p = q;
            out = v;
            return true;
        }
        shift += 7U;
        if (shift > 63U) return false;
    }
    return false;
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_DSTD_VARINT_H
