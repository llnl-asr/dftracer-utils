// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_FATAL_H
#define DFTRACER_UTILS_DSTD_FATAL_H

namespace dftracer::utils::dstd {

/// @brief Ends the process on a broken invariant.
///
/// Prints `what`, then `detail` when it is not null, then a stack trace to
/// stderr, and aborts. Every fatal path goes through here, so one breakpoint
/// stops on all of them.
[[noreturn]] void fatal(const char* what,
                        const char* detail = nullptr) noexcept;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_FATAL_H
