// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fmt.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dftracer::utils::dstd::fmt_detail {

namespace {

char* pad_zeros(char* first, char* last, std::size_t width, bool neg) noexcept {
    const std::size_t len = static_cast<std::size_t>(last - first);
    if (len >= width) return last;
    const std::size_t zeros = width - len;
    char* digits = first + (neg ? 1 : 0);
    std::memmove(digits + zeros, digits,
                 static_cast<std::size_t>(last - digits));
    std::memset(digits, '0', zeros);
    return last + zeros;
}

}  // namespace

char* write_integer(char* d, std::uint64_t mag, bool neg, Spec s) noexcept {
    char* first = d;
    if (neg) *d++ = '-';
    const bool hex = s.type == 'x' || s.type == 'X';
    d = dstd::to_chars(d, d + 24, mag, hex ? 16 : 10).ptr;
    if (s.type == 'X')
        for (char* p = first; p != d; ++p)
            if (*p >= 'a' && *p <= 'f') *p = static_cast<char>(*p - 'a' + 'A');
    return pad_zeros(first, d, s.width, neg);
}

char* write_fixed(char* d, std::size_t room, double v, Spec s) noexcept {
    char* end = to_chars_fixed(d, d + room, v, s.prec);
    if (end == nullptr) return d;
    if (!std::isfinite(v)) return end;
    return pad_zeros(d, end, s.width, std::signbit(v));
}

}  // namespace dftracer::utils::dstd::fmt_detail
