#ifndef DFTRACER_UTILS_CORE_COMMON_STR_FORMAT_H
#define DFTRACER_UTILS_CORE_COMMON_STR_FORMAT_H

#include <dftracer/utils/core/common/to_chars.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>

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

// Integral types (excluding bool/char, handled above) via std::to_chars - the
// integer overload is available on every supported platform (only the
// floating-point one is availability-gated, see to_chars.h).
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

// Type-safe concatenation: appends each argument to one string, routing numbers
// through to_chars (faster than std::to_string, portable unlike std::format).
// Strings/string_views/char append directly; integers/floats convert via
// to_chars; bool -> "true"/"false". No format string to parse.
//   str_cat("Cannot open ", path, ": errno=", errno)
template <typename... Args>
std::string str_cat(const Args&... args) {
    std::string out;
    (detail::str_append(out, args), ...);
    return out;
}

// printf-style formatting into a std::string. Portable (plain vsnprintf, no
// std::format / availability gates); use for cold, mixed-content messages. For
// literal '%' in the text, escape as "%%" (or use str_cat, which has no format
// string). vstring_format is the va_list core; string_format is the variadic
// front-end.
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

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_STR_FORMAT_H
