// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_FATAL_H
#define DFTRACER_UTILS_DSTD_FATAL_H

#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::dstd {

/// @brief Ends the process on a broken invariant.
///
/// Prints `what`, then `detail` when it is not empty, then a stack trace to
/// stderr, and aborts. Every fatal path goes through here, so one breakpoint
/// stops on all of them.
[[noreturn]] void fatal(StringView what, StringView detail = {}) noexcept;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_FATAL_H
