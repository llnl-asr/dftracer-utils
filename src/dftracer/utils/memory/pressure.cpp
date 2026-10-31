#include <dftracer/utils/memory/pressure.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace dftracer::utils::memory {

#ifdef __linux__

namespace {

constexpr const char* CGROUP_ROOT = "/sys/fs/cgroup";
constexpr std::size_t PATH_BYTES = 1024;
constexpr std::uint64_t NO_LIMIT = std::uint64_t{1} << 60;

enum class Version : std::uint8_t { None, V1, V2 };

struct Cgroup {
    Version version = Version::None;
    char base[PATH_BYTES];
    char dir[PATH_BYTES];
};

bool readable(const char* dir, const char* file) noexcept {
    char path[PATH_BYTES + 64];
    std::snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) return false;
    std::fclose(f);
    return true;
}

// The path of the cgroup in the hierarchy this process sees; "/" in a
// container that mounts only its own cgroup.
void read_cgroup_paths(char* v2, char* v1, std::size_t capacity) noexcept {
    v2[0] = '\0';
    v1[0] = '\0';
    FILE* f = std::fopen("/proc/self/cgroup", "r");
    if (f == nullptr) return;
    char line[PATH_BYTES];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::size_t n = std::strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        char* first = std::strchr(line, ':');
        if (first == nullptr) continue;
        char* second = std::strchr(first + 1, ':');
        if (second == nullptr) continue;
        *second = '\0';
        if (first[1] == '\0') {
            std::snprintf(v2, capacity, "%s", second + 1);
        } else {
            char* save = nullptr;
            for (char* c = strtok_r(first + 1, ",", &save); c != nullptr;
                 c = strtok_r(nullptr, ",", &save)) {
                if (std::strcmp(c, "memory") == 0) {
                    std::snprintf(v1, capacity, "%s", second + 1);
                    break;
                }
            }
        }
    }
    std::fclose(f);
}

void join(Cgroup& cg, const char* base, const char* path,
          const char* stat_file) noexcept {
    std::snprintf(cg.base, sizeof(cg.base), "%s", base);
    std::snprintf(cg.dir, sizeof(cg.dir), "%s%s", base,
                  std::strcmp(path, "/") == 0 ? "" : path);
    if (!readable(cg.dir, stat_file)) {
        std::snprintf(cg.dir, sizeof(cg.dir), "%s", base);
    }
}

Cgroup find_cgroup() noexcept {
    Cgroup cg;
    char v2[PATH_BYTES / 2];
    char v1[PATH_BYTES / 2];
    read_cgroup_paths(v2, v1, sizeof(v2));
    if (v2[0] != '\0') {
        join(cg, CGROUP_ROOT, v2, "memory.stat");
        if (readable(cg.dir, "memory.stat")) {
            cg.version = Version::V2;
            return cg;
        }
    }
    char v1_base[PATH_BYTES / 2];
    std::snprintf(v1_base, sizeof(v1_base), "%s/memory", CGROUP_ROOT);
    join(cg, v1_base, v1[0] != '\0' ? v1 : "/", "memory.stat");
    if (readable(cg.dir, "memory.stat")) cg.version = Version::V1;
    return cg;
}

bool key_is(const char* line, const char* key) noexcept {
    std::size_t n = std::strlen(key);
    return std::strncmp(line, key, n) == 0 && line[n] == ' ';
}

std::uint64_t value_of(const char* line, const char* key) noexcept {
    return std::strtoull(line + std::strlen(key) + 1, nullptr, 10);
}

std::size_t memory_from_stat(const Cgroup& cg) noexcept {
    char path[PATH_BYTES + 64];
    std::snprintf(path, sizeof(path), "%s/memory.stat", cg.dir);
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) return 0;
    const char* anon = cg.version == Version::V2 ? "anon" : "rss";
    std::uint64_t total = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (key_is(line, anon)) {
            total += value_of(line, anon);
        } else if (key_is(line, "shmem")) {
            total += value_of(line, "shmem");
        }
    }
    std::fclose(f);
    return static_cast<std::size_t>(total);
}

std::size_t memory_from_status() noexcept {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) return 0;
    std::uint64_t kib = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, "RssAnon:", 8) == 0 ||
            std::strncmp(line, "RssShmem:", 9) == 0) {
            kib += std::strtoull(std::strchr(line, ':') + 1, nullptr, 10);
        }
    }
    std::fclose(f);
    return static_cast<std::size_t>(kib * 1024);
}

std::uint64_t read_number(const char* dir, const char* file) noexcept {
    char path[PATH_BYTES + 64];
    std::snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) return 0;
    char text[64] = {};
    std::size_t n = std::fread(text, 1, sizeof(text) - 1, f);
    std::fclose(f);
    if (n == 0 || std::strncmp(text, "max", 3) == 0) return 0;
    return std::strtoull(text, nullptr, 10);
}

}  // namespace

std::size_t read_process_memory() noexcept { return memory_from_status(); }

std::size_t read_limit_memory() noexcept {
    Cgroup cg = find_cgroup();
    if (cg.version != Version::None) return memory_from_stat(cg);
    return memory_from_status();
}

std::size_t read_process_limit() noexcept {
    Cgroup cg = find_cgroup();
    if (cg.version == Version::None) return 0;
    const char* file =
        cg.version == Version::V2 ? "memory.max" : "memory.limit_in_bytes";
    std::size_t base_size = std::strlen(cg.base);
    while (true) {
        std::uint64_t limit = read_number(cg.dir, file);
        if (limit > 0 && limit < NO_LIMIT) {
            return static_cast<std::size_t>(limit);
        }
        std::size_t size = std::strlen(cg.dir);
        if (size <= base_size) return 0;
        char* slash = std::strrchr(cg.dir, '/');
        if (slash == nullptr) return 0;
        *slash = '\0';
    }
}

#elif defined(__APPLE__)

std::size_t read_process_memory() noexcept {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<std::size_t>(info.phys_footprint);
}

std::size_t read_limit_memory() noexcept { return read_process_memory(); }

std::size_t read_process_limit() noexcept { return 0; }

#else

std::size_t read_process_memory() noexcept { return 0; }
std::size_t read_limit_memory() noexcept { return 0; }
std::size_t read_process_limit() noexcept { return 0; }

#endif

}  // namespace dftracer::utils::memory
