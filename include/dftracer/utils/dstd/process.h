// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_PROCESS_H
#define DFTRACER_UTILS_DSTD_PROCESS_H

#include <cstdint>

namespace dftracer::utils::dstd {

/// True when a process with this id exists on this machine, also one that this
/// user may not signal. False when it does not exist or has been reaped.
bool process_is_alive(std::uint64_t pid) noexcept;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_PROCESS_H
