#ifndef DFTRACER_UTILS_BENCHMARKS_BENCH_COUNTERS_H
#define DFTRACER_UTILS_BENCHMARKS_BENCH_COUNTERS_H

#include <sys/resource.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#if defined(__APPLE__)
#include <libproc.h>
#include <unistd.h>
#elif defined(__linux__)
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

/**
 * Counts for bench executables, so a comparison does not depend on how busy
 * the machine is.
 *
 * instructions_retired() is the instructions the whole process retired, every
 * thread. It is unavailable (never faked) when the platform gives no counter:
 * on Linux when perf_event_open is refused, for example by
 * kernel.perf_event_paranoid. Call init_counters() once at the start of main,
 * before any thread starts, because the Linux counter follows only threads
 * that start after it is opened.
 *
 * allocations() counts calls of the global operator new. The memory manager
 * keeps no running count of its calls, and code from before the manager took
 * coroutine frames gets them from operator new, so this count is the same
 * kind of number on both sides of a comparison. Exactly one .cpp of an
 * executable defines DFTRACER_UTILS_BENCH_COUNT_NEW before including this
 * header. That replaces the global operator new and delete of the whole
 * executable with versions that count calls. Never define it in a library or in
 * the Python extension.
 *
 * peak_heap_bytes() is the peak resident set of the process so far. The
 * manager's own peak and reserved counters leave out the frames that older
 * code takes from operator new, so they cannot be compared across versions.
 */
namespace dftu::bench {

inline std::atomic<std::uint64_t> &new_calls() {
    static std::atomic<std::uint64_t> calls{0};
    return calls;
}

#if defined(__linux__)
inline int &perf_fd() {
    static int fd = -1;
    return fd;
}
#endif

inline void init_counters() {
#if defined(__linux__)
    perf_event_attr attr{};
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = PERF_COUNT_HW_INSTRUCTIONS;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.inherit = 1;
    perf_fd() =
        static_cast<int>(::syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0UL));
#endif
}

inline bool instructions_available() {
#if defined(__APPLE__)
    return true;
#elif defined(__linux__)
    return perf_fd() >= 0;
#else
    return false;
#endif
}

/** Instructions retired by the process so far; 0 when unavailable. */
inline std::uint64_t instructions_retired() {
#if defined(__APPLE__)
    rusage_info_v4 info{};
    if (::proc_pid_rusage(::getpid(), RUSAGE_INFO_V4,
                          reinterpret_cast<rusage_info_t *>(&info)) != 0) {
        return 0;
    }
    return info.ri_instructions;
#elif defined(__linux__)
    if (perf_fd() < 0) return 0;
    std::uint64_t value = 0;
    if (::read(perf_fd(), &value, sizeof(value)) !=
        static_cast<ssize_t>(sizeof(value))) {
        return 0;
    }
    return value;
#else
    return 0;
#endif
}

/** Calls of the global operator new so far. */
inline std::uint64_t allocations() {
    return new_calls().load(std::memory_order_relaxed);
}

/** Peak resident set of the process so far, in bytes. */
inline std::uint64_t peak_heap_bytes() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

}  // namespace dftu::bench

#ifdef DFTRACER_UTILS_BENCH_COUNT_NEW

void *operator new(std::size_t size) {
    dftu::bench::new_calls().fetch_add(1, std::memory_order_relaxed);
    void *p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}

void *operator new(std::size_t size, std::align_val_t align) {
    dftu::bench::new_calls().fetch_add(1, std::memory_order_relaxed);
    void *p = nullptr;
    std::size_t alignment = static_cast<std::size_t>(align);
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    if (::posix_memalign(&p, alignment, size == 0 ? 1 : size) != 0) {
        throw std::bad_alloc();
    }
    return p;
}

namespace dftu::bench {
__attribute__((noinline)) inline void free_block(void *p) noexcept {
    std::free(p);
}
}  // namespace dftu::bench

void operator delete(void *p) noexcept { dftu::bench::free_block(p); }
void operator delete(void *p, std::size_t) noexcept {
    dftu::bench::free_block(p);
}
void operator delete(void *p, std::align_val_t) noexcept {
    dftu::bench::free_block(p);
}
void operator delete(void *p, std::size_t, std::align_val_t) noexcept {
    dftu::bench::free_block(p);
}

#endif  // DFTRACER_UTILS_BENCH_COUNT_NEW

#endif  // DFTRACER_UTILS_BENCHMARKS_BENCH_COUNTERS_H
