// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_NUMBERS_H
#define DFTRACER_UTILS_DUQL_NUMBERS_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/variant.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <type_traits>
#include <variant>

namespace dftracer::utils::duql {

/// A JSON number as the widest exact type that holds it.
using Number = dstd::Variant<std::int64_t, std::uint64_t, double>;

namespace detail {

template <class T>
int order(T a, T b) {
    return a < b ? -1 : (b < a ? 1 : 0);
}

// Exact comparison of an integer with a double: the integer is never rounded
// to a double, so 2^53 + 1 does not equal 2^53.
inline dstd::Optional<int> cmp_num(std::int64_t a, double d) {
    constexpr double TWO_POW_63 = 9223372036854775808.0;
    if (std::isnan(d)) return std::nullopt;
    if (d >= TWO_POW_63) return -1;
    if (d < -TWO_POW_63) return 1;
    const double f = std::floor(d);
    const auto fi = static_cast<std::int64_t>(f);
    if (a != fi) return order(a, fi);
    return d > f ? -1 : 0;
}

inline dstd::Optional<int> cmp_num(std::uint64_t a, double d) {
    constexpr double TWO_POW_64 = 18446744073709551616.0;
    if (std::isnan(d)) return std::nullopt;
    if (d < 0) return 1;
    if (d >= TWO_POW_64) return -1;
    const double f = std::floor(d);
    const auto fu = static_cast<std::uint64_t>(f);
    if (a != fu) return order(a, fu);
    return d > f ? -1 : 0;
}

inline dstd::Optional<int> cmp_num(std::int64_t a, std::uint64_t b) {
    if (a < 0) return -1;
    return order(static_cast<std::uint64_t>(a), b);
}

inline dstd::Optional<int> flip(dstd::Optional<int> c) {
    if (c) return -*c;
    return c;
}

}  // namespace detail

/// -1/0/1 for a < b, a == b, a > b, exact across integer and double types;
/// nullopt when either is NaN.
template <class A, class B>
dstd::Optional<int> compare_numbers(A a, B b) {
    using detail::cmp_num;
    using detail::flip;
    using detail::order;
    if constexpr (std::is_same_v<A, B>) {
        if constexpr (std::is_same_v<A, double>)
            if (std::isnan(a) || std::isnan(b)) return std::nullopt;
        return order(a, b);
    } else if constexpr (std::is_same_v<A, double>) {
        return flip(cmp_num(b, a));
    } else if constexpr (std::is_same_v<B, double>) {
        return cmp_num(a, b);
    } else if constexpr (std::is_same_v<A, std::int64_t>) {
        return cmp_num(a, b);
    } else {
        return flip(cmp_num(b, a));
    }
}

inline dstd::Optional<int> compare_numbers(const Number& a, const Number& b) {
    return std::visit([](auto x, auto y) { return compare_numbers(x, y); }, a,
                      b);
}

/// Whether `c` can begin text that strtod accepts: a blank, a sign, a digit,
/// a point (a comma in a locale that uses one), or the first letter of inf and
/// nan. Anything else is not a number, and skipping strtod avoids its locale
/// lock.
inline bool may_start_double(char c) {
    switch (c) {
        case ' ':
        case '\t':
        case '\n':
        case '\v':
        case '\f':
        case '\r':
        case '+':
        case '-':
        case '.':
        case ',':
        case 'i':
        case 'I':
        case 'n':
        case 'N':
            return true;
        default:
            return c >= '0' && c <= '9';
    }
}

/// `text` as a number when all of it is one: an integer as int64, or uint64
/// above int64's range, anything else as a double.
inline dstd::Optional<Number> parse_number(dstd::StringView text) {
    const char* first = text.data();
    const char* last = first + text.size();
    if (std::int64_t i = 0;
        std::from_chars(first, last, i).ptr == last && !text.empty())
        return Number{i};
    if (std::uint64_t u = 0;
        std::from_chars(first, last, u).ptr == last && !text.empty())
        return Number{u};
    if (text.empty() || !may_start_double(text.front())) return std::nullopt;
    // strtod, not from_chars: the double overload is not on older macOS.
    const dstd::String copy(text);
    char* end = nullptr;
    const double d = std::strtod(copy.c_str(), &end);
    if (!copy.empty() && end == copy.c_str() + copy.size()) return Number{d};
    return std::nullopt;
}

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_NUMBERS_H
