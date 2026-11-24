// SPDX-License-Identifier: MIT
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/memory/cgroup.h>
#include <dftracer/utils/memory/heap_page.h>

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace dftracer::utils::memory {

static constexpr std::size_t FALLBACK_AVAILABLE_BYTES =
    1ULL * 1024 * 1024 * 1024;

static constexpr std::size_t CGROUP_LIMIT_SENTINEL =
    1ULL * 1024 * 1024 * 1024 * 1024;

static std::size_t read_size_at(std::string_view dir, std::string_view leaf) {
    const std::string text = read_cgroup_file(dir, leaf);
    if (text.empty() || std::strncmp(text.c_str(), "max", 3) == 0) return 0;
    char *end = nullptr;
    unsigned long long val = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str()) return 0;
    return static_cast<std::size_t>(val);
}

static std::size_t cgroup_v2_limit_at(std::string_view cg_path) {
    std::size_t room = 0;
    for_each_cgroup_dir("/sys/fs/cgroup", cg_path, [&](std::string_view dir) {
        std::size_t max_mem = read_size_at(dir, "/memory.max");
        if (max_mem == 0 || max_mem >= CGROUP_LIMIT_SENTINEL) return false;
        std::size_t current = read_size_at(dir, "/memory.current");
        room = current >= max_mem ? 0 : max_mem - current;
        return true;
    });
    return room;
}

static std::size_t cgroup_v1_limit_at(std::string_view cg_path) {
    std::size_t room = 0;
    for_each_cgroup_dir(
        "/sys/fs/cgroup/memory", cg_path, [&](std::string_view dir) {
            std::size_t limit = read_size_at(dir, "/memory.limit_in_bytes");
            if (limit == 0 || limit >= CGROUP_LIMIT_SENTINEL) return false;
            std::size_t usage = read_size_at(dir, "/memory.usage_in_bytes");
            room = usage >= limit ? 0 : limit - usage;
            return true;
        });
    return room;
}

static std::size_t try_cgroups_v2() {
    std::string v2_path =
        read_self_cgroup_paths("/proc/self/cgroup", "memory").v2;
    if (v2_path.empty()) v2_path = "/";
    return cgroup_v2_limit_at(v2_path);
}

static std::size_t try_cgroups_v1() {
    std::string v1_path =
        read_self_cgroup_paths("/proc/self/cgroup", "memory").v1;
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

// macOS: the free, inactive and speculative pages, which is what the
// kernel would hand out without swapping.
static std::size_t try_darwin() {
#ifdef __APPLE__
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vm),
                          &count) != KERN_SUCCESS)
        return 0;
    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS) return 0;
    const std::uint64_t pages = static_cast<std::uint64_t>(vm.free_count) +
                                vm.inactive_count + vm.speculative_count;
    return static_cast<std::size_t>(pages * page);
#else
    return 0;
#endif
}

std::size_t detect_available_memory() {
    std::size_t avail = try_cgroups_v2();
    if (avail > 0) return avail;
    avail = try_cgroups_v1();
    if (avail > 0) return avail;
    avail = try_proc_meminfo();
    if (avail > 0) return avail;
    avail = try_darwin();
    if (avail > 0) return avail;
    return FALLBACK_AVAILABLE_BYTES;
}

std::uint64_t resolve_spill_budget(std::uint64_t configured) {
    if (configured == 0) {
        const std::uint64_t b =
            static_cast<std::uint64_t>(detect_available_memory() / 3);
        return b < MIN_MEMORY_BUDGET_BYTES ? MIN_MEMORY_BUDGET_BYTES : b;
    }
    return configured;  // explicit bytes, or NO_SPILL_BUDGET passthrough
}

namespace {

// Live scopes per power of two of their budget; the lowest bucket in use sets
// the idle cap. No lock, so a fork cannot leave one held in the child. A writer
// that sees the version move under it computes the cap again, so the last
// store follows the last change.
constexpr int BUDGET_BUCKETS = 64;
std::atomic<std::uint32_t> g_budget_scopes[BUDGET_BUCKETS];
std::atomic<std::uint64_t> g_budget_version{0};

int budget_bucket(std::uint64_t budget) noexcept {
    if (budget == 0 || budget / 4 >= detail::IDLE_BACKED_CAP) return -1;
    return static_cast<int>(std::bit_width(budget)) - 1;
}

void apply_idle_cap() noexcept {
    for (;;) {
        const std::uint64_t seen =
            g_budget_version.load(std::memory_order_acquire);
        std::size_t cap = detail::IDLE_BACKED_CAP;
        for (int b = 0; b < BUDGET_BUCKETS; ++b) {
            if (g_budget_scopes[b].load(std::memory_order_acquire) != 0) {
                cap = (std::size_t{1} << b) / 4;
                break;
            }
        }
        detail::page_set_idle_cap(cap);
        if (g_budget_version.load(std::memory_order_acquire) == seen) return;
    }
}

void change_scopes(int bucket, bool add) noexcept {
    if (add) {
        g_budget_scopes[bucket].fetch_add(1, std::memory_order_acq_rel);
    } else {
        g_budget_scopes[bucket].fetch_sub(1, std::memory_order_acq_rel);
    }
    g_budget_version.fetch_add(1, std::memory_order_acq_rel);
    apply_idle_cap();
}

}  // namespace

JobBudgetScope::JobBudgetScope(std::uint64_t budget) noexcept
    : bucket_(budget_bucket(budget)) {
    if (bucket_ >= 0) change_scopes(bucket_, true);
}

JobBudgetScope::JobBudgetScope(JobBudgetScope &&other) noexcept
    : bucket_(std::exchange(other.bucket_, -1)) {}

JobBudgetScope::~JobBudgetScope() {
    if (bucket_ >= 0) change_scopes(bucket_, false);
}

}  // namespace dftracer::utils::memory
