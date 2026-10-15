#ifndef DFTRACER_UTILS_COMMON_STR_FORMAT_H
#define DFTRACER_UTILS_COMMON_STR_FORMAT_H

#include <dftracer/utils/common/to_chars.h>

#include <charconv>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace dftracer::utils {

namespace detail {

inline void str_append(std::string& out, std::string_view s) { out.append(s); }
inline void str_append(std::string& out, const char* s) {
    if (s) out.append(s);
}
inline void str_append(std::string& out, const std::string& s) {
    out.append(s);
}
inline void str_append(std::string& out, char c) { out.push_back(c); }
inline void str_append(std::string& out, bool b) {
    out.append(b ? "true" : "false");
}

/// Integral types (excluding bool/char, handled above) via std::to_chars - the
/// integer overload is available on every supported platform (only the
/// floating-point one is availability-gated, see to_chars.h).
template <typename T,
          std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> &&
                               !std::is_same_v<T, char>,
                           int> = 0>
void str_append(std::string& out, T v) {
    char buf[24];
    auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    if (ec == std::errc{}) out.append(buf, p);
}

inline void str_append(std::string& out, double v) {
    char buf[32];
    char* end = to_chars_double(buf, buf + sizeof(buf), v);
    if (end) out.append(buf, end);
}
inline void str_append(std::string& out, float v) {
    str_append(out, static_cast<double>(v));
}

}  // namespace detail

/// Type-safe concatenation: appends each argument to one string, routing
/// numbers through to_chars (faster than std::to_string, portable unlike
/// std::format). Strings/string_views/char append directly; integers/floats
/// convert via to_chars; bool -> "true"/"false". No format string to parse.
///   str_cat("Cannot open ", path, ": errno=", errno)
template <typename... Args>
std::string str_cat(const Args&... args) {
    std::string out;
    (detail::str_append(out, args), ...);
    return out;
}

/// printf-style formatting into a std::string. Portable (plain vsnprintf, no
/// std::format / availability gates); use for cold, mixed-content messages. For
/// literal '%' in the text, escape as "%%" (or use str_cat, which has no format
/// string). vstring_format is the va_list core; string_format is the variadic
/// front-end.
__attribute__((__format__(__printf__, 1, 0))) inline std::string vstring_format(
    const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(nullptr, 0, fmt, ap2);
    va_end(ap2);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n) + 1, '\0');
    std::vsnprintf(s.data(), s.size(), fmt, ap);
    s.resize(static_cast<std::size_t>(n));
    return s;
}

__attribute__((__format__(__printf__, 1, 2))) inline std::string string_format(
    const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = vstring_format(fmt, ap);
    va_end(ap);
    return s;
}

/// Human-readable byte count, e.g. "1.5 MB" or "3.0 MB/s". `per_suffix` appends
/// a rate unit (e.g. "/s"); `precision` sets the fractional digits.
inline std::string human_bytes(double value, const char* per_suffix = "",
                               int precision = 1) {
    static const char* const UNITS[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int i = 0;
    while (value >= 1024.0 && i < 5) {
        value /= 1024.0;
        ++i;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f %s%s", precision, value, UNITS[i],
                  per_suffix);
    return buf;
}

namespace detail {

constexpr bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

constexpr char to_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr std::string_view trim_ws(std::string_view s) {
    while (!s.empty() && is_ascii_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ascii_space(s.back())) s.remove_suffix(1);
    return s;
}

constexpr bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (to_lower_ascii(a[i]) != to_lower_ascii(b[i])) return false;
    }
    return true;
}

// Parse a leading decimal number and return {value, byte offset past it}, or
// nullopt if there is no number. Runtime uses strtod (libc++ does not implement
// the from_chars float overload); the constexpr branch is hand-rolled since
// strtod is not constexpr. strtod is unbounded, so the numeric prefix is copied
// into a NUL-terminated buffer first.
constexpr std::optional<std::pair<double, std::size_t>> parse_leading_number(
    std::string_view s) {
    if (!std::is_constant_evaluated()) {
        std::size_t n = 0;
        if (n < s.size() && (s[n] == '+' || s[n] == '-')) ++n;
        std::size_t digits = 0;
        while (n < s.size() && s[n] >= '0' && s[n] <= '9') ++n, ++digits;
        if (n < s.size() && s[n] == '.') {
            ++n;
            while (n < s.size() && s[n] >= '0' && s[n] <= '9') ++n, ++digits;
        }
        if (n < s.size() && (s[n] == 'e' || s[n] == 'E')) {
            std::size_t e = n + 1;
            if (e < s.size() && (s[e] == '+' || s[e] == '-')) ++e;
            std::size_t exp_digits = 0;
            while (e < s.size() && s[e] >= '0' && s[e] <= '9')
                ++e, ++exp_digits;
            if (exp_digits > 0) n = e;
        }
        if (digits == 0) return std::nullopt;
        char buf[64];
        if (n >= sizeof(buf)) return std::nullopt;
        for (std::size_t i = 0; i < n; ++i) buf[i] = s[i];
        buf[n] = '\0';
        char* end = nullptr;
        const double value = std::strtod(buf, &end);
        if (end == buf) return std::nullopt;
        return std::make_pair(value, static_cast<std::size_t>(end - buf));
    }
    std::size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        ++i;
    }
    double value = 0.0;
    bool any_digit = false;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        value = value * 10.0 + (s[i] - '0');
        any_digit = true;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        double scale = 0.1;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
            value += (s[i] - '0') * scale;
            scale *= 0.1;
            any_digit = true;
        }
    }
    if (!any_digit) return std::nullopt;
    if (neg) value = -value;
    return std::make_pair(value, i);
}

// Returns {value, trimmed unit token} or nullopt when there is no number.
constexpr std::optional<std::pair<double, std::string_view>> split_value_unit(
    std::string_view s) {
    s = trim_ws(s);
    auto num = parse_leading_number(s);
    if (!num) return std::nullopt;
    return std::make_pair(num->first, trim_ws(s.substr(num->second)));
}

}  // namespace detail

/// Parse a human-readable size such as "512", "64KB", "1.5GiB", "8kb" into a
/// byte count. Byte units are always 1024-based, so "KB" and "KiB" are the
/// same. The trailing character is case-sensitive: 'B' is bytes, 'b' is bits
/// (result divided by 8); an 'i' before it is accepted and ignored. The
/// magnitude prefix (k/m/g/t/p) is case-insensitive. A bare number is bytes.
/// Returns nullopt on malformed input or an unknown unit.
constexpr std::optional<std::uint64_t> parse_bytes(std::string_view s) {
    auto parsed = detail::split_value_unit(s);
    if (!parsed) return std::nullopt;
    double value = parsed->first;
    std::string_view unit = parsed->second;
    if (value < 0.0) return std::nullopt;

    bool bits = false;
    if (!unit.empty() && unit.back() == 'B') {
        unit.remove_suffix(1);
    } else if (!unit.empty() && unit.back() == 'b') {
        bits = true;
        unit.remove_suffix(1);
    }
    if (!unit.empty() && (unit.back() == 'i' || unit.back() == 'I'))
        unit.remove_suffix(1);

    int power = 0;
    if (unit.empty()) {
        power = 0;
    } else if (unit.size() == 1) {
        switch (detail::to_lower_ascii(unit.front())) {
            case 'k':
                power = 1;
                break;
            case 'm':
                power = 2;
                break;
            case 'g':
                power = 3;
                break;
            case 't':
                power = 4;
                break;
            case 'p':
                power = 5;
                break;
            default:
                return std::nullopt;
        }
    } else {
        return std::nullopt;
    }

    double mult = 1.0;
    for (int i = 0; i < power; ++i) mult *= 1024.0;
    double bytes = value * mult;
    if (bits) bytes /= 8.0;
    return static_cast<std::uint64_t>(bytes);
}

/// Parse a byte size where a bare number is measured in `native_bytes` units
/// (e.g. 1024*1024 for a legacy value expressed in MB) while a unit-suffixed
/// value ("512KB") is absolute. Lets a legacy MB argument accept human-readable
/// strings without changing what its bare numbers mean. Returns nullopt on
/// malformed input.
constexpr std::optional<std::uint64_t> parse_bytes_as(
    std::string_view s, std::uint64_t native_bytes) {
    auto parsed = detail::split_value_unit(s);
    if (!parsed) return std::nullopt;
    if (parsed->second.empty()) {
        if (parsed->first < 0.0) return std::nullopt;
        return static_cast<std::uint64_t>(parsed->first *
                                          static_cast<double>(native_bytes));
    }
    return parse_bytes(s);
}

/// Parse a human-readable duration such as "30", "500ms", "1.5h", "2m" into
/// seconds. A bare number is seconds. Recognized units: ns, us, ms, s/sec,
/// m/min, h/hr, d/day. Returns nullopt on malformed input or an unknown unit.
constexpr std::optional<double> parse_duration_seconds(std::string_view s) {
    auto parsed = detail::split_value_unit(s);
    if (!parsed) return std::nullopt;
    double value = parsed->first;
    std::string_view unit = parsed->second;
    if (value < 0.0) return std::nullopt;
    using detail::iequals;
    if (unit.empty() || iequals(unit, "s") || iequals(unit, "sec") ||
        iequals(unit, "secs"))
        return value;
    if (iequals(unit, "ns")) return value / 1e9;
    if (iequals(unit, "us")) return value / 1e6;
    if (iequals(unit, "ms")) return value / 1e3;
    if (iequals(unit, "m") || iequals(unit, "min") || iequals(unit, "mins"))
        return value * 60.0;
    if (iequals(unit, "h") || iequals(unit, "hr") || iequals(unit, "hrs"))
        return value * 3600.0;
    if (iequals(unit, "d") || iequals(unit, "day") || iequals(unit, "days"))
        return value * 86400.0;
    return std::nullopt;
}

/// Parse a duration where a bare number is measured in the caller's native unit
/// (`native_per_second`, e.g. 1e3 for a value expressed in ms) and a
/// unit-suffixed value ("5m", "500ns") is converted into that same native unit.
/// Lets a legacy ms/us argument accept human-readable strings without changing
/// what its bare numbers mean. Returns nullopt on malformed input.
constexpr std::optional<double> parse_duration_as(std::string_view s,
                                                  double native_per_second) {
    auto parsed = detail::split_value_unit(s);
    if (!parsed) return std::nullopt;
    if (parsed->second.empty()) {
        if (parsed->first < 0.0) return std::nullopt;
        return parsed->first;
    }
    auto seconds = parse_duration_seconds(s);
    if (!seconds) return std::nullopt;
    return *seconds * native_per_second;
}

static_assert(parse_bytes("512") == 512);
static_assert(parse_bytes("64KB") == std::uint64_t{64} * 1024);
static_assert(parse_bytes("64KiB") == std::uint64_t{64} * 1024);
static_assert(parse_bytes("1.5GiB") ==
              static_cast<std::uint64_t>(1.5 * 1024 * 1024 * 1024));
static_assert(parse_bytes("1GB") == std::uint64_t{1024} * 1024 * 1024);
static_assert(parse_bytes("8kb") == 1024);   // 8 * 1024 bits / 8
static_assert(parse_bytes("8Kib") == 1024);  // same, 'i' ignored
static_assert(parse_bytes("8b") == 1);
static_assert(parse_bytes("8B") == 8);
static_assert(!parse_bytes("12xb").has_value());
static_assert(parse_duration_seconds("30") == 30.0);
static_assert(parse_duration_seconds("5m") == 300.0);
static_assert(parse_duration_seconds("1.5h") == 5400.0);
static_assert(!parse_duration_seconds("10q").has_value());

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_COMMON_STR_FORMAT_H
