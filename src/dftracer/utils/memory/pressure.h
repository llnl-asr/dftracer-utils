// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_MEMORY_PRESSURE_H
#define DFTRACER_UTILS_MEMORY_PRESSURE_H

#include <cstddef>

namespace dftracer::utils::memory {

/// Anonymous plus shared memory of this process in bytes: RssAnon and
/// RssShmem of /proc/self/status on Linux, `phys_footprint` on macOS. Zero
/// when unreadable. Opens files and allocates nothing through this library.
std::size_t read_process_memory() noexcept;

/// The memory counted against read_process_limit(), in bytes. In a cgroup it is
/// the anonymous plus shared memory of every process of the group, else
/// read_process_memory(). Zero when unreadable.
std::size_t read_limit_memory() noexcept;

/// The memory limit of the process cgroup, or of the nearest ancestor that
/// has one. Zero when there is none, always on macOS.
std::size_t read_process_limit() noexcept;

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_PRESSURE_H
