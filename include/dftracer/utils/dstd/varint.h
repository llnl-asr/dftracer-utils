// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_VARINT_H
#define DFTRACER_UTILS_DSTD_VARINT_H

#include <dftracer/utils/dstd/little_endian.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::dstd {

// LEB128 for u64: seven bits per byte, low group first, the high bit marks a
// following byte.

inline constexpr std::size_t VARINT_MAX_BYTES = 10;

/// Bytes that write_varint and append_varint produce for v.
inline constexpr std::size_t varint_size(std::uint64_t v) {
    std::size_t size = 1;
    while (v >= 0x80U) {
        v >>= 7U;
        ++size;
    }
    return size;
}

/// Moves the sign into the low bit, so a small magnitude costs one byte.
inline constexpr std::uint64_t zigzag_encode(std::int64_t v) {
    return (static_cast<std::uint64_t>(v) << 1U) ^
           static_cast<std::uint64_t>(v >> 63);
}

inline constexpr std::int64_t zigzag_decode(std::uint64_t z) {
    return static_cast<std::int64_t>((z >> 1U) ^ (0U - (z & 1U)));
}

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
/// true. A truncated or over-long value returns false and leaves p and out
/// unchanged.
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

/// Decodes one varint at the cursor. A truncated or over-long value returns
/// false and leaves the cursor where it was.
inline bool read_varint(LeReader& r, std::uint64_t& out) {
    const std::uint8_t* at = r.position();
    const std::uint8_t* p = at;
    if (!read_varint(p, at + r.remaining(), out)) return false;
    r.take(static_cast<std::size_t>(p - at));
    return true;
}

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_VARINT_H
