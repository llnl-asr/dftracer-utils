// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_MEMORY_CGROUP_H
#define DFTRACER_UTILS_MEMORY_CGROUP_H

#include <cstddef>
#include <string>
#include <string_view>

namespace dftracer::utils::memory {

struct CgroupPaths {
    /// The path of the "0::" line, empty when the process is on no v2 tree.
    std::string v2;
    /// The path of the line that lists the v1 controller, empty when none.
    std::string v1;
};

/// Split a /proc/<pid>/cgroup listing. `v1_controller` names the v1 hierarchy
/// to pick ("memory", "cpu", "pids").
CgroupPaths parse_cgroup_paths(std::string_view listing,
                               std::string_view v1_controller);

/// parse_cgroup_paths of the file at `proc_file`; both empty when it cannot be
/// read.
CgroupPaths read_self_cgroup_paths(std::string_view proc_file,
                                   std::string_view v1_controller);

/// The text of `dir` + `leaf` without trailing white space, empty when the
/// file cannot be read. Reads at most 255 bytes.
std::string read_cgroup_file(std::string_view dir, std::string_view leaf);

/// Calls `visit(dir)` for `root` + `cg_path`, then for each parent directory
/// up to `root`, and stops at the first call that returns true. Returns
/// whether one did.
template <class Visit>
bool for_each_cgroup_dir(std::string_view root, std::string_view cg_path,
                         Visit &&visit) {
    std::string dir(root);
    dir.append(cg_path);
    while (true) {
        if (visit(std::string_view(dir))) return true;
        if (dir.size() <= root.size()) return false;
        std::size_t slash = dir.find_last_of('/');
        if (slash == std::string::npos || slash < root.size()) return false;
        dir.resize(slash);
    }
}

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_CGROUP_H
