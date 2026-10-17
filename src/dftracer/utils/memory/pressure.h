#ifndef DFTRACER_UTILS_MEMORY_PRESSURE_H
#define DFTRACER_UTILS_MEMORY_PRESSURE_H

#include <cstddef>

namespace dftracer::utils::memory {

/// Anonymous plus shared memory of the process in bytes: cgroup v2 `anon` and
/// `shmem`, cgroup v1 `rss` and `shmem`, else RssAnon and RssShmem of
/// /proc/self/status; `phys_footprint` on macOS. Zero when unreadable. Opens
/// files and allocates nothing through this library.
std::size_t read_process_memory() noexcept;

/// The memory limit of the process cgroup, or of the nearest ancestor that
/// has one. Zero when there is none, always on macOS.
std::size_t read_process_limit() noexcept;

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_PRESSURE_H
