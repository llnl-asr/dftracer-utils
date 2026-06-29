#ifndef DFTRACER_UTILS_CORE_COMMON_TO_CHARS_H
#define DFTRACER_UTILS_CORE_COMMON_TO_CHARS_H

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <system_error>

#if defined(__APPLE__) && (!defined(__MAC_OS_X_VERSION_MIN_REQUIRED) || \
                           __MAC_OS_X_VERSION_MIN_REQUIRED < 130300)
// Apple libc++ availability-gates floating-point std::to_chars to macOS
// 13.3+; fall back to snprintf on older deployment targets (cibuildwheel
// arm64 default is 11.0).
#define DFTRACER_UTILS_FP_TO_CHARS_UNAVAILABLE 1
#include <cstdio>
#endif

namespace dftracer::utils {

/// Format a double into [first, last). Returns past-the-end pointer on
/// success, nullptr on overflow/error. Uses std::to_chars where available
/// for shortest round-trip; falls back to snprintf("%.17g", ...) on
/// platforms where libc++ availability-gates the floating-point overload.
inline char* to_chars_double(char* first, char* last, double v) noexcept {
    if (last <= first) return nullptr;
#ifdef DFTRACER_UTILS_FP_TO_CHARS_UNAVAILABLE
    const std::size_t cap = static_cast<std::size_t>(last - first);
    const int n = std::snprintf(first, cap, "%.17g", v);
    if (n <= 0 || static_cast<std::size_t>(n) >= cap) return nullptr;
    return first + n;
#else
    auto [p, ec] = std::to_chars(first, last, v);
    return ec == std::errc{} ? p : nullptr;
#endif
}

/// Format an unsigned 64-bit integer into [first, last). Returns past-the-end
/// pointer on success, nullptr on overflow. The integer std::to_chars overload
/// is available on every supported platform (only the floating-point one is
/// availability-gated), so no fallback is needed.
inline char* to_chars_u64(char* first, char* last, std::uint64_t v) noexcept {
    auto [p, ec] = std::to_chars(first, last, v);
    return ec == std::errc{} ? p : nullptr;
}

}  // namespace dftracer::utils

#endif
