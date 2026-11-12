// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_HASH_HEX64_H
#define DFTRACER_UTILS_DSTD_HASH_HEX64_H

#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dftracer::utils::dstd::hash {

/// dftracer writes file and host hashes as exactly 16 lowercase hex digits, so
/// they are 64-bit values. Anything outside that form has no round trip and
/// must keep its original text.
inline constexpr std::size_t HEX64_DIGITS = 16;

inline Optional<std::uint64_t> parse_hex64(StringView sv) {
    if (sv.size() != HEX64_DIGITS) return nullopt;
    std::uint64_t v = 0;
    for (char c : sv) {
        std::uint64_t d;
        if (is_ascii_digit(c))
            d = static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = static_cast<std::uint64_t>(c - 'a' + 10);
        else
            return nullopt;
        v = (v << 4) | d;
    }
    return v;
}

/// Writes HEX64_DIGITS digits into `out`, which must have room.
inline void format_hex64(std::uint64_t v, char* out) {
    static constexpr char DIGITS[] = "0123456789abcdef";
    for (std::size_t i = 0; i < HEX64_DIGITS; ++i) {
        out[HEX64_DIGITS - 1 - i] = DIGITS[v & 0xF];
        v >>= 4;
    }
}

inline String format_hex64(std::uint64_t v) {
    String s(HEX64_DIGITS, '0');
    format_hex64(v, s.data());
    return s;
}

}  // namespace dftracer::utils::dstd::hash

#endif  // DFTRACER_UTILS_DSTD_HASH_HEX64_H
