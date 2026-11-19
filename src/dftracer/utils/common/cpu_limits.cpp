// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/cpu_limits.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/memory/cgroup.h>

#include <algorithm>

#if defined(__linux__)
#include <sched.h>
#endif

namespace dftracer::utils::common {

namespace {

// A cgroup file holds a number, or "max" and "-1" for no limit.
dstd::Optional<std::size_t> read_count(dstd::StringView dir,
                                       dstd::StringView leaf) {
    const auto text = memory::read_cgroup_file(dir, leaf);
    if (text.empty()) return dstd::nullopt;
    return dstd::from_chars<std::size_t>(dstd::StringView(text));
}

// Every level of the path counts, so a parent's quota limits its children.
// `level` returns the limit a directory sets, or 0 for none. The result is the
// smallest, 0 when no level sets one.
template <class Level>
std::size_t smallest_limit(dstd::StringView root, dstd::StringView cg_path,
                           Level&& level) {
    std::size_t smallest = 0;
    memory::for_each_cgroup_dir(root, cg_path, [&](dstd::StringView dir) {
        const std::size_t n = level(dir);
        if (n != 0 && (smallest == 0 || n < smallest)) smallest = n;
        return false;
    });
    return smallest;
}

std::size_t ceil_div(std::size_t a, std::size_t b) { return (a + b - 1) / b; }

std::size_t v2_cpu_level(dstd::StringView dir) {
    const auto text = memory::read_cgroup_file(dir, "/cpu.max");
    const std::size_t space = text.find(' ');
    if (space == dstd::StringView::npos) return 0;
    const auto quota =
        dstd::from_chars<std::size_t>(dstd::StringView(text.data(), space));
    const auto period = dstd::from_chars<std::size_t>(
        dstd::StringView(text.data() + space + 1, text.size() - space - 1));
    if (!quota || !period || *quota == 0 || *period == 0) return 0;
    return ceil_div(*quota, *period);
}

std::size_t v1_cpu_level(dstd::StringView dir) {
    const auto quota = read_count(dir, "/cpu.cfs_quota_us");
    const auto period = read_count(dir, "/cpu.cfs_period_us");
    if (!quota || !period || *quota == 0 || *period == 0) return 0;
    return ceil_div(*quota, *period);
}

// Room in one level, plus one so that a full group (0 left) is told apart from
// "no limit" by smallest_limit.
std::size_t pids_level(dstd::StringView dir) {
    const auto max = read_count(dir, "/pids.max");
    if (!max) return 0;
    const auto current = read_count(dir, "/pids.current").value_or(0);
    return (*max > current ? *max - current : 0) + 1;
}

}  // namespace

std::size_t cgroup_cpu_limit(const CgroupFs& fs) {
    const memory::CgroupPaths v2 =
        memory::read_self_cgroup_paths(fs.proc_cgroup, "cpu");
    if (!v2.v2.empty()) {
        const std::size_t n = smallest_limit(fs.root, v2.v2, v2_cpu_level);
        if (n != 0) return n;
    }
    if (!v2.v1.empty()) {
        const dstd::String root = dstd::fmt<"{}/cpu">(fs.root);
        return smallest_limit(root, v2.v1, v1_cpu_level);
    }
    return 0;
}

std::size_t cgroup_thread_room(const CgroupFs& fs) {
    const memory::CgroupPaths paths =
        memory::read_self_cgroup_paths(fs.proc_cgroup, "pids");
    std::size_t room = 0;
    if (!paths.v2.empty()) room = smallest_limit(fs.root, paths.v2, pids_level);
    if (room == 0 && !paths.v1.empty()) {
        const dstd::String root = dstd::fmt<"{}/pids">(fs.root);
        room = smallest_limit(root, paths.v1, pids_level);
    }
    return room == 0 ? UNLIMITED_THREADS : room - 1;
}

std::size_t thread_room() {
#if defined(__linux__)
    return cgroup_thread_room();
#else
    return UNLIMITED_THREADS;
#endif
}

std::size_t default_worker_threads(std::size_t cpus, std::size_t room) {
    if (room == UNLIMITED_THREADS) return cpus;
    const std::size_t free =
        room > FIXED_RUNTIME_THREADS ? room - FIXED_RUNTIME_THREADS : 0;
    return std::min(cpus, std::max<std::size_t>(free / 2, 1));
}

std::size_t default_io_threads(std::size_t workers, std::size_t room) {
    if (room == UNLIMITED_THREADS) return workers;
    const std::size_t used = FIXED_RUNTIME_THREADS + workers;
    const std::size_t free = room > used ? room - used : 0;
    return std::min(workers, std::max<std::size_t>(free, 1));
}

std::size_t hardware_concurrency() {
    if (const auto v = Env::get<std::uint64_t>("DFTRACER_UTILS_HW_CONCURRENCY");
        v && *v > 0)
        return static_cast<std::size_t>(*v);
    std::size_t n = dstd::Thread::hardware_concurrency();
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        const int allowed = CPU_COUNT(&set);
        if (allowed > 0) n = static_cast<std::size_t>(allowed);
    }
    // The quota is read once: callers ask often and a group's quota rarely
    // changes under a running process.
    static const std::size_t quota = cgroup_cpu_limit();
    if (quota != 0) n = n == 0 ? quota : std::min(n, quota);
#endif
    return n == 0 ? 1 : n;
}

}  // namespace dftracer::utils::common
