// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_CPU_LIMITS_H
#define DFTRACER_UTILS_COMMON_CPU_LIMITS_H

#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <limits>

namespace dftracer::utils::common {

/// What @ref thread_room returns when nothing limits the thread count.
inline constexpr std::size_t UNLIMITED_THREADS =
    std::numeric_limits<std::size_t>::max();

/// Where the readers look. The defaults are the live system; a test points
/// them at a copy of the cgroup tree.
struct CgroupFs {
    dstd::StringView proc_cgroup = "/proc/self/cgroup";
    dstd::StringView root = "/sys/fs/cgroup";
};

/// The CPUs the CPU quota of the process's cgroup allows, rounded up: the
/// smallest quota along the path from the process's group to the root (v2
/// `cpu.max`, else v1 `cpu.cfs_quota_us` over `cpu.cfs_period_us`). 0 when no
/// group sets one.
std::size_t cgroup_cpu_limit(const CgroupFs& fs = {});

/// The threads the process's cgroup still lets it start: the smallest
/// `pids.max - pids.current` along the path to the root (v2, or the v1 pids
/// controller). UNLIMITED_THREADS when no group sets a limit.
std::size_t cgroup_thread_room(const CgroupFs& fs = {});

/// The number of new threads this process may still start, or
/// UNLIMITED_THREADS. It reads the cgroup pids limit on each call. It cannot
/// see RLIMIT_NPROC, which counts every thread of the user.
std::size_t thread_room();

/// Threads a runtime starts besides its workers and I/O threads: the monitor,
/// the system monitor, the timer, the I/O completion thread and the like.
inline constexpr std::size_t FIXED_RUNTIME_THREADS = 8;

/// The default worker count for `cpus` usable CPUs when `room` threads may
/// still start: `cpus` when the room is UNLIMITED_THREADS, else at most the
/// workers that fit, with an equal I/O pool and the fixed threads, and at
/// least 1.
std::size_t default_worker_threads(std::size_t cpus, std::size_t room);

/// The default I/O thread count for `workers` workers when `room` threads may
/// still start: `workers` when the room is UNLIMITED_THREADS, else at most
/// what fits beside the workers and the fixed threads, and at least 1.
std::size_t default_io_threads(std::size_t workers, std::size_t room);

}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_CPU_LIMITS_H
