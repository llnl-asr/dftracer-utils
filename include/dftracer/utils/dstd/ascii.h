// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_ASCII_H
#define DFTRACER_UTILS_DSTD_ASCII_H

#include <dftracer/utils/dstd/string.h>

#include <cstddef>

namespace dftracer::utils::dstd {

/// ASCII character classes. They ignore the locale and are false for every
/// byte >= 0x80.
constexpr bool is_ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool is_ascii_lower(char c) noexcept { return c >= 'a' && c <= 'z'; }
constexpr bool is_ascii_upper(char c) noexcept { return c >= 'A' && c <= 'Z'; }
constexpr bool is_ascii_alpha(char c) noexcept {
    const char folded = static_cast<char>(c | 0x20);
    return folded >= 'a' && folded <= 'z';
}
constexpr bool is_ascii_alnum(char c) noexcept {
    return is_ascii_alpha(c) || is_ascii_digit(c);
}
constexpr bool is_ascii_xdigit(char c) noexcept {
    const char folded = static_cast<char>(c | 0x20);
    return is_ascii_digit(c) || (folded >= 'a' && folded <= 'f');
}
/// The C whitespace set: space, \t, \n, \v, \f, \r.
constexpr bool is_ascii_space(char c) noexcept {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

/// Case conversion of ASCII letters; every other byte is returned unchanged.
constexpr char to_ascii_lower(char c) noexcept {
    return is_ascii_upper(c) ? static_cast<char>(c | 0x20) : c;
}
constexpr char to_ascii_upper(char c) noexcept {
    return is_ascii_lower(c) ? static_cast<char>(c & ~0x20) : c;
}

/// True when @p a and @p b have the same size and equal bytes after
/// @ref to_ascii_lower.
constexpr bool ascii_iequals(StringView a, StringView b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (to_ascii_lower(a[i]) != to_ascii_lower(b[i])) return false;
    }
    return true;
}

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_ASCII_H
