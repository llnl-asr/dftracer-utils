// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_CLOCK_H
#define DFTRACER_UTILS_DSTD_CLOCK_H

#include <time.h>

#include <chrono>
#include <cstdint>

namespace dftracer::utils::dstd {

/// Nanoseconds on the steady clock. Only differences are meaningful: the epoch
/// is arbitrary and the value is not comparable across processes.
inline std::uint64_t steady_now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// Nanoseconds since the Unix epoch on the system clock. It steps, so it is for
/// a value stamped into output, never for a difference.
inline std::uint64_t system_now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

/// Nanoseconds on the POSIX clock @p id, for a clock a syscall takes by id.
/// @p id must be one the platform knows.
inline std::uint64_t posix_clock_now_ns(clockid_t id) noexcept {
    timespec ts{};
    ::clock_gettime(id, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

/// Nanoseconds on a coarse monotonic clock of the same kind as
/// @ref steady_now_ns, at a lower cost: one tick a millisecond or slower, and
/// no kernel entry on Linux. For an age check that tolerates a late tick.
inline std::uint64_t coarse_now_ns() noexcept {
#if defined(__APPLE__)
    return posix_clock_now_ns(CLOCK_MONOTONIC_RAW_APPROX);
#elif defined(CLOCK_MONOTONIC_COARSE)
    return posix_clock_now_ns(CLOCK_MONOTONIC_COARSE);
#else
    return posix_clock_now_ns(CLOCK_MONOTONIC);
#endif
}

/// The steady clock's current time. The same reading as @ref steady_now_ns, for
/// code that does arithmetic on a deadline.
inline std::chrono::steady_clock::time_point steady_now() noexcept {
    return std::chrono::steady_clock::now();
}

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_CLOCK_H
