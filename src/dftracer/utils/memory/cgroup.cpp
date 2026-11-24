// SPDX-License-Identifier: MIT
#include <dftracer/utils/memory/cgroup.h>

#include <cstdio>
#include <cstring>

namespace dftracer::utils::memory {

CgroupPaths parse_cgroup_paths(std::string_view listing,
                               std::string_view v1_controller) {
    CgroupPaths out;
    std::size_t pos = 0;
    while (pos < listing.size()) {
        std::size_t eol = listing.find('\n', pos);
        if (eol == std::string_view::npos) eol = listing.size();
        std::string_view line = listing.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() >= 3 && line.starts_with("0::")) {
            out.v2 = std::string(line.substr(3));
            continue;
        }
        const std::size_t first = line.find(':');
        if (first == std::string_view::npos) continue;
        const std::size_t second = line.find(':', first + 1);
        if (second == std::string_view::npos) continue;
        const std::string_view controllers =
            line.substr(first + 1, second - first - 1);
        std::size_t start = 0;
        while (start <= controllers.size()) {
            std::size_t comma = controllers.find(',', start);
            std::size_t end =
                comma == std::string_view::npos ? controllers.size() : comma;
            if (controllers.substr(start, end - start) == v1_controller) {
                out.v1 = std::string(line.substr(second + 1));
                break;
            }
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
    }
    return out;
}

CgroupPaths read_self_cgroup_paths(std::string_view proc_file,
                                   std::string_view v1_controller) {
    const std::string path(proc_file);
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    std::string listing;
    char buf[1024];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) listing.append(buf, n);
    std::fclose(f);
    return parse_cgroup_paths(listing, v1_controller);
}

// The memory library links nothing of ours, so no dstd::fmt here.
std::string read_cgroup_file(std::string_view dir, std::string_view leaf) {
    std::string path;
    path.reserve(dir.size() + leaf.size());
    path.append(dir);
    path.append(leaf);
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    char buf[256];
    const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::string text(buf, n);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                             text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
    return text;
}

}  // namespace dftracer::utils::memory
