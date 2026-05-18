#include <dftracer/utils/core/common/memory_budget.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace dftracer::utils {

static constexpr std::size_t FALLBACK_AVAILABLE_BYTES =
    1ULL * 1024 * 1024 * 1024;

static std::size_t read_size_from_file(const char *path) {
    FILE *f = std::fopen(path, "r");
    if (!f) return 0;
    char buf[64];
    std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';
    if (std::strncmp(buf, "max", 3) == 0) return 0;
    char *end = nullptr;
    unsigned long long val = std::strtoull(buf, &end, 10);
    if (end == buf) return 0;
    return static_cast<std::size_t>(val);
}

static constexpr std::size_t CGROUP_LIMIT_SENTINEL =
    1ULL * 1024 * 1024 * 1024 * 1024;

static void read_self_cgroup_paths(std::string &v2_path, std::string &v1_path) {
    FILE *f = std::fopen("/proc/self/cgroup", "r");
    if (!f) return;
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        std::size_t n = std::strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n >= 3 && line[0] == '0' && line[1] == ':' && line[2] == ':') {
            v2_path = line + 3;
            continue;
        }
        char *first = std::strchr(line, ':');
        if (!first) continue;
        char *second = std::strchr(first + 1, ':');
        if (!second) continue;
        std::string controllers(first + 1, second - first - 1);
        std::size_t start = 0;
        while (start <= controllers.size()) {
            std::size_t comma = controllers.find(',', start);
            std::size_t end =
                (comma == std::string::npos) ? controllers.size() : comma;
            if (controllers.compare(start, end - start, "memory") == 0) {
                v1_path = second + 1;
                break;
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    std::fclose(f);
}

static std::size_t cgroup_v2_limit_at(const std::string &cg_path) {
    std::string base = "/sys/fs/cgroup" + cg_path;
    std::string dir = base;
    while (true) {
        std::size_t max_mem =
            read_size_from_file((dir + "/memory.max").c_str());
        if (max_mem > 0 && max_mem < CGROUP_LIMIT_SENTINEL) {
            std::size_t current =
                read_size_from_file((dir + "/memory.current").c_str());
            if (current >= max_mem) return 0;
            return max_mem - current;
        }
        if (dir.size() <= std::strlen("/sys/fs/cgroup")) break;
        std::size_t slash = dir.find_last_of('/');
        if (slash == std::string::npos || slash < std::strlen("/sys/fs/cgroup"))
            break;
        dir.resize(slash);
    }
    return 0;
}

static std::size_t cgroup_v1_limit_at(const std::string &cg_path) {
    std::string base = "/sys/fs/cgroup/memory" + cg_path;
    std::string dir = base;
    while (true) {
        std::size_t limit =
            read_size_from_file((dir + "/memory.limit_in_bytes").c_str());
        if (limit > 0 && limit < CGROUP_LIMIT_SENTINEL) {
            std::size_t usage =
                read_size_from_file((dir + "/memory.usage_in_bytes").c_str());
            if (usage >= limit) return 0;
            return limit - usage;
        }
        if (dir.size() <= std::strlen("/sys/fs/cgroup/memory")) break;
        std::size_t slash = dir.find_last_of('/');
        if (slash == std::string::npos ||
            slash < std::strlen("/sys/fs/cgroup/memory"))
            break;
        dir.resize(slash);
    }
    return 0;
}

static std::size_t try_cgroups_v2() {
    std::string v2_path, v1_path;
    read_self_cgroup_paths(v2_path, v1_path);
    if (v2_path.empty()) v2_path = "/";
    return cgroup_v2_limit_at(v2_path);
}

static std::size_t try_cgroups_v1() {
    std::string v2_path, v1_path;
    read_self_cgroup_paths(v2_path, v1_path);
    if (v1_path.empty()) v1_path = "/";
    return cgroup_v1_limit_at(v1_path);
}

static std::size_t try_proc_meminfo() {
    FILE *f = std::fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "MemAvailable:", 13) == 0) {
            char *p = line + 13;
            while (*p == ' ') ++p;
            char *end = nullptr;
            unsigned long long val = std::strtoull(p, &end, 10);
            std::fclose(f);
            return static_cast<std::size_t>(val) * 1024;
        }
    }
    std::fclose(f);
    return 0;
}

std::size_t detect_available_memory() {
    std::size_t avail = try_cgroups_v2();
    if (avail > 0) return avail;
    avail = try_cgroups_v1();
    if (avail > 0) return avail;
    avail = try_proc_meminfo();
    if (avail > 0) return avail;
    return FALLBACK_AVAILABLE_BYTES;
}

std::size_t compute_memory_budget(std::size_t user_override_bytes) {
    if (user_override_bytes > 0) return user_override_bytes;
    std::size_t avail = detect_available_memory();
    std::size_t budget = avail * DEFAULT_MEMORY_BUDGET_FRACTION_PERCENT / 100;
    return std::max(budget, MIN_MEMORY_BUDGET_BYTES);
}

std::size_t compute_channel_capacity(std::size_t memory_budget_bytes,
                                     std::size_t estimated_batch_bytes,
                                     std::size_t num_workers) {
    std::size_t from_budget =
        memory_budget_bytes / std::max(estimated_batch_bytes, std::size_t(1));
    std::size_t minimum = std::max(num_workers * 2, std::size_t(4));
    return std::max(from_budget, minimum);
}

std::size_t compute_file_batch_size(std::size_t memory_budget_bytes,
                                    std::size_t estimated_file_bytes,
                                    std::size_t min_files) {
    std::size_t from_budget =
        memory_budget_bytes / std::max(estimated_file_bytes, std::size_t(1));
    return std::max(from_budget, std::max(min_files, std::size_t(1)));
}

std::size_t estimate_per_file_bytes(const std::vector<std::size_t> &file_sizes,
                                    std::size_t user_override_bytes) {
    if (user_override_bytes > 0) return user_override_bytes;
    if (file_sizes.empty()) return MIN_PER_FILE_PEAK_BYTES;

    const std::size_t total = file_sizes.size();
    const std::size_t sample_count = std::min(total, PER_FILE_SAMPLE_LIMIT);
    const std::size_t stride = std::max(total / sample_count, std::size_t(1));

    std::vector<std::size_t> sizes;
    sizes.reserve(sample_count);
    for (std::size_t i = 0; i < total && sizes.size() < sample_count;
         i += stride) {
        if (file_sizes[i] > 0) sizes.push_back(file_sizes[i]);
    }

    if (sizes.empty()) return MIN_PER_FILE_PEAK_BYTES;

    std::size_t idx = (sizes.size() * 95) / 100;
    if (idx >= sizes.size()) idx = sizes.size() - 1;
    std::nth_element(sizes.begin(), sizes.begin() + idx, sizes.end());
    const std::size_t p95 = sizes[idx];

    std::size_t estimate = p95 * PER_FILE_EXPANSION_FACTOR;
    estimate = std::max(estimate, MIN_PER_FILE_PEAK_BYTES);
    estimate = std::min(estimate, MAX_PER_FILE_PEAK_BYTES);
    return estimate;
}

}  // namespace dftracer::utils
