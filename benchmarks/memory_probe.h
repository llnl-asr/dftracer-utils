#ifndef DFTRACER_UTILS_BENCHMARKS_MEMORY_PROBE_H
#define DFTRACER_UTILS_BENCHMARKS_MEMORY_PROBE_H

#include <sys/resource.h>

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#ifdef __APPLE__
#include <mach/mach.h>
#include <malloc/malloc.h>
#else
#include <malloc.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#endif

/**
 * Memory readings and an allocation counter for bench executables.
 *
 * Exactly one .cpp of an executable defines DFTU_MEMORY_PROBE_DEFINE_NEW
 * before including this header. That replaces the global operator new and
 * delete of the whole executable with counting versions. Never define it in a
 * library or in the Python extension.
 */
namespace dftu::memory_probe {

/**
 * All sizes are bytes. `anon` is RssAnon on Linux and phys_footprint on macOS.
 * `cgroup_anon` is memory.stat anon + shmem of the cgroup v2 this process is
 * in; it is valid only when `cgroup_present` is true (never on macOS).
 */
struct Reading {
    std::uint64_t current_rss = 0;
    std::uint64_t peak_rss = 0;
    std::uint64_t anon = 0;
    std::uint64_t cgroup_anon = 0;
    bool cgroup_present = false;
};

constexpr std::size_t HISTOGRAM_BUCKETS = 65;

inline std::atomic<std::int64_t>& live_bytes_counter() {
    static std::atomic<std::int64_t> counter{0};
    return counter;
}

inline std::atomic<std::uint64_t>* histogram_counters() {
    static std::atomic<std::uint64_t> buckets[HISTOGRAM_BUCKETS]{};
    return buckets;
}

/** Bytes the allocator holds for live `new` blocks, as it reports them. */
inline std::int64_t live_bytes() {
    return live_bytes_counter().load(std::memory_order_relaxed);
}

/** Count of `new` calls with a request size in [2^(b-1), 2^b); bucket 0 is size
 * 0. */
inline std::uint64_t histogram_count(std::size_t bucket) {
    return histogram_counters()[bucket].load(std::memory_order_relaxed);
}

inline std::size_t histogram_bucket(std::size_t size) {
    return static_cast<std::size_t>(std::bit_width(size));
}

#ifdef __APPLE__

inline Reading read() {
    Reading r;
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS) {
        r.current_rss = info.resident_size;
        r.anon = info.phys_footprint;
    }
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    r.peak_rss = static_cast<std::uint64_t>(u.ru_maxrss);
    return r;
}

inline std::size_t block_size(void* p) { return malloc_size(p); }

#else

namespace detail {

inline std::uint64_t kib_field(const std::string& line, const char* key) {
    const std::size_t n = std::strlen(key);
    if (line.compare(0, n, key) != 0) return 0;
    return std::strtoull(line.c_str() + n, nullptr, 10) * 1024;
}

inline bool cgroup_anon_bytes(std::uint64_t& out) {
    std::ifstream self("/proc/self/cgroup");
    std::string line;
    std::string path;
    while (std::getline(self, line)) {
        if (line.compare(0, 3, "0::") == 0) path = line.substr(3);
    }
    if (path.empty()) return false;
    std::ifstream stat("/sys/fs/cgroup" + path + "/memory.stat");
    if (!stat) return false;
    std::uint64_t anon = 0;
    std::uint64_t shmem = 0;
    bool have_anon = false;
    bool have_shmem = false;
    std::string key;
    std::uint64_t value = 0;
    while (stat >> key >> value) {
        if (key == "anon") {
            anon = value;
            have_anon = true;
        } else if (key == "shmem") {
            shmem = value;
            have_shmem = true;
        }
    }
    out = anon + shmem;
    return have_anon && have_shmem;
}

}  // namespace detail

inline Reading read() {
    Reading r;
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (const auto v = detail::kib_field(line, "VmRSS:")) r.current_rss = v;
        if (const auto v = detail::kib_field(line, "RssAnon:")) r.anon = v;
    }
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    r.peak_rss = static_cast<std::uint64_t>(u.ru_maxrss) * 1024;
    r.cgroup_present = detail::cgroup_anon_bytes(r.cgroup_anon);
    return r;
}

inline std::size_t block_size(void* p) { return malloc_usable_size(p); }

#endif

namespace detail {

// Unsized delete cannot know the request size, so both sides use the size the
// allocator reports for the block.
inline void* counted_alloc(std::size_t size, std::size_t align) {
    void* p = nullptr;
    if (align <= alignof(std::max_align_t)) {
        p = std::malloc(size == 0 ? 1 : size);
    } else if (posix_memalign(&p, align, size == 0 ? 1 : size) != 0) {
        p = nullptr;
    }
    if (p == nullptr) return nullptr;
    live_bytes_counter().fetch_add(static_cast<std::int64_t>(block_size(p)),
                                   std::memory_order_relaxed);
    histogram_counters()[histogram_bucket(size)].fetch_add(
        1, std::memory_order_relaxed);
    return p;
}

inline void counted_free(void* p) noexcept {
    if (p == nullptr) return;
    live_bytes_counter().fetch_sub(static_cast<std::int64_t>(block_size(p)),
                                   std::memory_order_relaxed);
    std::free(p);
}

inline void* counted_alloc_or_throw(std::size_t size, std::size_t align) {
    void* p = counted_alloc(size, align);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}

}  // namespace detail

}  // namespace dftu::memory_probe

#ifdef DFTU_MEMORY_PROBE_DEFINE_NEW

namespace mp_detail = dftu::memory_probe::detail;

void* operator new(std::size_t size) {
    return mp_detail::counted_alloc_or_throw(size, alignof(std::max_align_t));
}
void* operator new[](std::size_t size) {
    return mp_detail::counted_alloc_or_throw(size, alignof(std::max_align_t));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return mp_detail::counted_alloc(size, alignof(std::max_align_t));
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return mp_detail::counted_alloc(size, alignof(std::max_align_t));
}
void* operator new(std::size_t size, std::align_val_t align) {
    return mp_detail::counted_alloc_or_throw(size,
                                             static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align) {
    return mp_detail::counted_alloc_or_throw(size,
                                             static_cast<std::size_t>(align));
}
void* operator new(std::size_t size, std::align_val_t align,
                   const std::nothrow_t&) noexcept {
    return mp_detail::counted_alloc(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align,
                     const std::nothrow_t&) noexcept {
    return mp_detail::counted_alloc(size, static_cast<std::size_t>(align));
}

void operator delete(void* p) noexcept { mp_detail::counted_free(p); }
void operator delete[](void* p) noexcept { mp_detail::counted_free(p); }
void operator delete(void* p, std::size_t) noexcept {
    mp_detail::counted_free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    mp_detail::counted_free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    mp_detail::counted_free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    mp_detail::counted_free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
    mp_detail::counted_free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    mp_detail::counted_free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    mp_detail::counted_free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    mp_detail::counted_free(p);
}
void operator delete(void* p, std::align_val_t,
                     const std::nothrow_t&) noexcept {
    mp_detail::counted_free(p);
}
void operator delete[](void* p, std::align_val_t,
                       const std::nothrow_t&) noexcept {
    mp_detail::counted_free(p);
}

#endif

#endif
