#ifndef DFTRACER_UTILS_MEMORY_PRESSURE_H
#define DFTRACER_UTILS_MEMORY_PRESSURE_H

#include <cstddef>

namespace dftracer::utils::memory {

/// Anonymous plus shared memory of this process in bytes: RssAnon and
/// RssShmem of /proc/self/status on Linux, `phys_footprint` on macOS. Zero
/// when unreadable. Opens files and allocates nothing through this library.
std::size_t read_process_memory() noexcept;

/// The memory counted against read_process_limit(), in bytes: in a cgroup,
/// its anonymous plus shared memory (cgroup v2 `anon` and `shmem`, v1 `rss`
/// and `shmem`), which takes in every process of the group; else
/// read_process_memory(). Zero when unreadable.
std::size_t read_limit_memory() noexcept;

/// The memory limit of the process cgroup, or of the nearest ancestor that
/// has one. Zero when there is none, always on macOS.
std::size_t read_process_limit() noexcept;

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_PRESSURE_H
