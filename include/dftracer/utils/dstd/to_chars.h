// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_TO_CHARS_H
#define DFTRACER_UTILS_DSTD_TO_CHARS_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>
#include <type_traits>

#if defined(__APPLE__) && (!defined(__MAC_OS_X_VERSION_MIN_REQUIRED) || \
                           __MAC_OS_X_VERSION_MIN_REQUIRED < 130300)
// Apple libc++ availability-gates floating-point std::to_chars to macOS
// 13.3+; fall back to snprintf on older deployment targets (cibuildwheel
// arm64 default is 11.0).
#define DFTRACER_UTILS_FP_TO_CHARS_UNAVAILABLE 1
#endif

// libc++ has no floating-point std::from_chars before LLVM 20, whatever the
// deployment target.
#if defined(DFTRACER_UTILS_FP_TO_CHARS_UNAVAILABLE) || \
    (defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 200000)
#define DFTRACER_UTILS_FP_FROM_CHARS_UNAVAILABLE 1
#endif

namespace dftracer::utils::dstd {

/// "nan" (never signed), "inf" or "-inf" into [first, last), the same text on
/// every platform. Returns past-the-end pointer, nullptr when it does not fit.
inline char* to_chars_nonfinite(char* first, char* last, double v) noexcept {
    const char* text = std::isnan(v) ? "nan" : (v < 0 ? "-inf" : "inf");
    const std::size_t n = std::strlen(text);
    if (static_cast<std::size_t>(last - first) < n) return nullptr;
    std::memcpy(first, text, n);
    return first + n;
}

/// Format a double into [first, last). Returns past-the-end pointer on
/// success, nullptr on overflow/error. Uses std::to_chars where available
/// for shortest round-trip; falls back to snprintf("%.17g", ...) on
/// platforms where libc++ availability-gates the floating-point overload.
inline char* to_chars_double(char* first, char* last, double v) noexcept {
    if (last <= first) return nullptr;
    if (!std::isfinite(v))
        return to_chars_nonfinite(first, last, static_cast<double>(v));
#ifdef DFTRACER_UTILS_FP_TO_CHARS_UNAVAILABLE
    // The shortest of %.15g, %.16g and %.17g that reads back as `v`: the
    // same digits std::to_chars gives.
    const std::size_t cap = static_cast<std::size_t>(last - first);
    for (int digits = 15; digits <= 17; ++digits) {
        char buf[32];
        const int n = std::snprintf(buf, sizeof(buf), "%.*g", digits, v);
        if (n <= 0 || static_cast<std::size_t>(n) >= cap) return nullptr;
        if (digits == 17 || std::strtod(buf, nullptr) == v) {
            std::memcpy(first, buf, static_cast<std::size_t>(n));
            return first + n;
        }
    }
    return nullptr;
#else
    auto [p, ec] = std::to_chars(first, last, v);
    return ec == std::errc{} ? p : nullptr;
#endif
}

/// `v` in shortest round-trip form ("2.5", not "2.500000").
inline String double_text(double v) {
    char buf[32];
    char* end = to_chars_double(buf, buf + sizeof(buf), v);
    return end ? String(buf, end) : String();
}

/// Format a float into [first, last), the shortest form that reads back as
/// `v`. Returns past-the-end pointer on success, nullptr on overflow/error.
/// Falls back to snprintf where libc++ gates the floating-point overload.
inline char* to_chars_float(char* first, char* last, float v) noexcept {
    if (last <= first) return nullptr;
    if (!std::isfinite(v))
        return to_chars_nonfinite(first, last, static_cast<double>(v));
#ifdef DFTRACER_UTILS_FP_TO_CHARS_UNAVAILABLE
    const std::size_t cap = static_cast<std::size_t>(last - first);
    for (int digits = 6; digits <= 9; ++digits) {
        char buf[32];
        const int n = std::snprintf(buf, sizeof(buf), "%.*g", digits,
                                    static_cast<double>(v));
        if (n <= 0 || static_cast<std::size_t>(n) >= cap) return nullptr;
        if (digits == 9 || std::strtof(buf, nullptr) == v) {
            std::memcpy(first, buf, static_cast<std::size_t>(n));
            return first + n;
        }
    }
    return nullptr;
#else
    auto [p, ec] = std::to_chars(first, last, v);
    return ec == std::errc{} ? p : nullptr;
#endif
}

/// `v` in shortest round-trip form for a float ("0.1", not "0.100000001").
inline String float_text(float v) {
    char buf[32];
    char* end = to_chars_float(buf, buf + sizeof(buf), v);
    return end ? String(buf, end) : String();
}

/// Format an unsigned 64-bit integer into [first, last). Returns past-the-end
/// pointer on success, nullptr on overflow. The integer std::to_chars overload
/// is available on every supported platform (only the floating-point one is
/// availability-gated), so no fallback is needed.
inline char* to_chars_u64(char* first, char* last, std::uint64_t v) noexcept {
    auto [p, ec] = std::to_chars(first, last, v);
    return ec == std::errc{} ? p : nullptr;
}

/// Signed sibling of to_chars_u64; the integer overload is always available.
inline char* to_chars_i64(char* first, char* last, std::int64_t v) noexcept {
    auto [p, ec] = std::to_chars(first, last, v);
    return ec == std::errc{} ? p : nullptr;
}

/// The decimal text of an integer.
template <class I>
inline String int_text(I v) {
    static_assert(std::is_integral_v<I>,
                  "the float std::to_chars needs macOS 13.3; use fixed_text");
    char buf[24];
    auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    return ec == std::errc{} ? String(buf, p) : String();
}

/// `v` as std::to_string gives it: fixed notation with six decimals.
inline String fixed_text(double v) {
    const int n = std::snprintf(nullptr, 0, "%f", v);
    String out(static_cast<std::size_t>(n), '\0');
    std::snprintf(out.data(), out.size() + 1, "%f", v);
    return out;
}

/// Parse a double from [first, last) like std::from_chars: no leading
/// whitespace or '+', `ptr` at the first byte not consumed, `ec` set when no
/// number starts there. Falls back to strtod on a copy where libc++ lacks the
/// floating-point overload.
inline std::from_chars_result from_chars_double(const char* first,
                                                const char* last,
                                                double& out) noexcept {
#ifdef DFTRACER_UTILS_FP_FROM_CHARS_UNAVAILABLE
    if (first == last || *first == ' ' || *first == '+')
        return {first, std::errc::invalid_argument};
    char buf[64];
    const std::size_t n = static_cast<std::size_t>(last - first);
    const std::size_t len = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
    for (std::size_t i = 0; i < len; ++i) buf[i] = first[i];
    buf[len] = 0;
    char* end = nullptr;
    const double v = std::strtod(buf, &end);
    if (end == buf) return {first, std::errc::invalid_argument};
    out = v;
    return {first + (end - buf), std::errc{}};
#else
    return std::from_chars(first, last, out);
#endif
}

template <class T>
inline constexpr bool IS_CHARS_NUMBER =
    (std::is_integral_v<T> && !std::is_same_v<T, bool>) ||
    std::is_same_v<T, float> || std::is_same_v<T, double>;

using ToCharsResult = std::to_chars_result;
using FromCharsResult = std::from_chars_result;

/// std::to_chars for integers, and the shortest form that reads back for a
/// float or double on every platform, where std::to_chars for floating point
/// is gated on older macOS. On a short buffer `ec` is value_too_large.
template <class T>
    requires IS_CHARS_NUMBER<T>
inline ToCharsResult to_chars(char* first, char* last, T v) noexcept {
    if constexpr (std::is_integral_v<T>) {
        return std::to_chars(first, last, v);
    } else {
        char* p = std::is_same_v<T, float>
                      ? to_chars_float(first, last, static_cast<float>(v))
                      : to_chars_double(first, last, static_cast<double>(v));
        if (p == nullptr) return {last, std::errc::value_too_large};
        return {p, std::errc{}};
    }
}

/// std::to_chars for an integer in `base` 2 to 36, lowercase digits.
template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
inline ToCharsResult to_chars(char* first, char* last, T v, int base) noexcept {
    return std::to_chars(first, last, v, base);
}

/// `v` in fixed notation with `precision` decimals, exact as printf("%.*f")
/// is. Returns past-the-end pointer on success, nullptr when [first, last) is
/// too short. Falls back to snprintf where libc++ gates the floating-point
/// overload.
inline char* to_chars_fixed(char* first, char* last, double v,
                            int precision) noexcept {
    if (last <= first) return nullptr;
    if (!std::isfinite(v))
        return to_chars_nonfinite(first, last, static_cast<double>(v));
#ifdef DFTRACER_UTILS_FP_TO_CHARS_UNAVAILABLE
    const std::size_t cap = static_cast<std::size_t>(last - first);
    const int n = std::snprintf(first, cap, "%.*f", precision, v);
    return n > 0 && static_cast<std::size_t>(n) < cap ? first + n : nullptr;
#else
    auto [p, ec] =
        std::to_chars(first, last, v, std::chars_format::fixed, precision);
    return ec == std::errc{} ? p : nullptr;
#endif
}

/// `v` with `precision` significant digits, as printf("%.*g") gives it: the
/// shorter of fixed and scientific notation, trailing zeros removed. Returns
/// past-the-end pointer on success, nullptr when [first, last) is too short.
/// Falls back to snprintf where libc++ gates the floating-point overload.
inline char* to_chars_general(char* first, char* last, double v,
                              int precision) noexcept {
    if (last <= first) return nullptr;
    if (!std::isfinite(v))
        return to_chars_nonfinite(first, last, static_cast<double>(v));
#ifdef DFTRACER_UTILS_FP_TO_CHARS_UNAVAILABLE
    const std::size_t cap = static_cast<std::size_t>(last - first);
    const int n = std::snprintf(first, cap, "%.*g", precision, v);
    return n > 0 && static_cast<std::size_t>(n) < cap ? first + n : nullptr;
#else
    auto [p, ec] =
        std::to_chars(first, last, v, std::chars_format::general, precision);
    return ec == std::errc{} ? p : nullptr;
#endif
}

/// std::from_chars for integers, float and double on every platform: no
/// leading whitespace or '+', `ptr` at the first byte not consumed, `ec` set
/// when no number starts there or the value is out of range for T.
template <class T>
    requires IS_CHARS_NUMBER<T>
inline FromCharsResult from_chars(const char* first, const char* last,
                                  T& out) noexcept {
    if constexpr (std::is_integral_v<T>) {
        return std::from_chars(first, last, out);
    } else if constexpr (std::is_same_v<T, double>) {
        return from_chars_double(first, last, out);
    } else {
        double d = 0;
        const FromCharsResult r = from_chars_double(first, last, d);
        if (r.ec != std::errc{}) return r;
        if (d > static_cast<double>(std::numeric_limits<float>::max()) ||
            d < -static_cast<double>(std::numeric_limits<float>::max()))
            return {r.ptr, std::errc::result_out_of_range};
        out = static_cast<float>(d);
        return r;
    }
}

/// The decimal text of `v`, as std::to_string gives it.
template <class T>
    requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
inline String to_string(T v) {
    if constexpr (std::is_integral_v<T>) {
        return int_text(v);
    } else {
        return fixed_text(static_cast<double>(v));
    }
}

/// The number that is all of `text`, or nothing when `text` is not exactly
/// one number of type T. As in std::from_chars, leading whitespace, a leading
/// '+' and trailing bytes make it nothing, and so does an out-of-range value.
template <class T>
    requires IS_CHARS_NUMBER<T>
inline Optional<T> from_chars(StringView text) {
    const char* first = text.data();
    const char* last = first + text.size();
    T value{};
    const FromCharsResult r = from_chars(first, last, value);
    if (r.ec != std::errc{} || r.ptr != last || first == last) return nullopt;
    return value;
}

}  // namespace dftracer::utils::dstd

#endif
