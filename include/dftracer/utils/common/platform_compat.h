// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_PLATFORM_COMPAT_H
#define DFTRACER_UTILS_COMMON_PLATFORM_COMPAT_H

// Cross-platform compatibility definitions

#ifdef _WIN32
// Windows specific includes and definitions
#include <fcntl.h>
#include <io.h>

// Map POSIX functions to Windows equivalents
#define fseeko _fseeki64
#define ftello _ftelli64
#define popen _popen
#define pclose _pclose
#define fileno _fileno
#define stat _stat64

// For large file support on Windows
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#else
// POSIX systems (Linux, macOS, etc.)
#include <unistd.h>

// Enable large file support on 32-bit systems
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

// Ensure we have the large file variants
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif

#endif

// Memory alignment constants for optimal performance.
// For false-sharing avoidance, use the spatial prefetch unit size (not just
// the L1 cache line) since adjacent-line prefetchers can cause cross-core
// invalidation even on separate cache lines.
//
// HPC targets: Intel Xeon, AMD EPYC, Apple Silicon, ARM Graviton,
// Fujitsu A64FX (Fugaku), IBM POWER.
#if defined(__A64FX__)
// Fujitsu A64FX (Fugaku): 256-byte cache lines
#define DFTRACER_UTILS_CACHE_LINE_SIZE 256
#define DFTRACER_UTILS_OPTIMAL_ALIGNMENT 256
#elif defined(__APPLE__) || defined(__ppc64__) || defined(__PPC64__) || \
    defined(_ARCH_PPC64)
// Apple Silicon: 128-byte cache lines
// IBM POWER9/10: 128-byte cache lines
#define DFTRACER_UTILS_CACHE_LINE_SIZE 128
#define DFTRACER_UTILS_OPTIMAL_ALIGNMENT 128
#elif defined(__x86_64__) || defined(_M_X64)
// Intel/AMD x86_64: 64-byte cache lines, 128-byte spatial prefetch pairs.
// Use 128 to avoid false sharing from adjacent-line prefetcher.
#define DFTRACER_UTILS_CACHE_LINE_SIZE 64
#define DFTRACER_UTILS_OPTIMAL_ALIGNMENT 128
#elif defined(__aarch64__)
// ARM64 Linux (Graviton, Neoverse, etc.): 64-byte cache lines
#define DFTRACER_UTILS_CACHE_LINE_SIZE 64
#define DFTRACER_UTILS_OPTIMAL_ALIGNMENT 64
#else
// Conservative default
#define DFTRACER_UTILS_CACHE_LINE_SIZE 64
#define DFTRACER_UTILS_OPTIMAL_ALIGNMENT 128
#endif

/// Convenience macro for aligned buffer declarations
#define DFTRACER_UTILS_ALIGNED_BUFFER(type, name, size) \
    alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT) type name[size]

#include <dftracer/utils/common/env.h>
#include <dftracer/utils/dstd/thread.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#if defined(__linux__)
#include <sched.h>
#endif

namespace dftracer::utils::common {

/// CPUs this process may run on (its affinity mask on Linux, else the cores
/// the machine reports), never 0. Overridable with
/// DFTRACER_UTILS_HW_CONCURRENCY.
inline std::size_t hardware_concurrency() {
    if (const auto v = Env::get<std::uint64_t>("DFTRACER_UTILS_HW_CONCURRENCY");
        v && *v > 0)
        return static_cast<std::size_t>(*v);
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        const int allowed = CPU_COUNT(&set);
        if (allowed > 0) return static_cast<std::size_t>(allowed);
    }
#endif
    const auto n = dstd::Thread::hardware_concurrency();
    return n == 0 ? 1u : static_cast<std::size_t>(n);
}

/// Worker threads available to the calling thread, 1 when it is not running
/// on an executor. This is what the runtime was configured with, not what
/// the machine has; size a fan-out with this.
std::size_t available_parallelism() noexcept;

}  // namespace dftracer::utils::common

// ThreadSanitizer annotations for custom synchronization primitives.
#if defined(__SANITIZE_THREAD__)
#define DFTRACER_UTILS_TSAN_ENABLED 1
#endif
#if !defined(DFTRACER_UTILS_TSAN_ENABLED) && defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DFTRACER_UTILS_TSAN_ENABLED 1
#endif
#endif

#if defined(DFTRACER_UTILS_TSAN_ENABLED)
#if __has_include(<sanitizer/tsan_interface.h>)
#include <sanitizer/tsan_interface.h>
#else
extern "C" void __tsan_acquire(void *);
extern "C" void __tsan_release(void *);
#endif
#define DFTRACER_UTILS_TSAN_ACQUIRE(addr) __tsan_acquire(addr)
#define DFTRACER_UTILS_TSAN_RELEASE(addr) __tsan_release(addr)
#else
#define DFTRACER_UTILS_TSAN_ACQUIRE(addr) ((void)0)
#define DFTRACER_UTILS_TSAN_RELEASE(addr) ((void)0)
#endif

// Turns off clang's -Wthread-safety for one function that takes a lock and
// leaves it to another, such as the prepare and child handlers of a fork.
#if defined(__clang__)
#define DFTRACER_UTILS_NO_THREAD_SAFETY_ANALYSIS \
    __attribute__((no_thread_safety_analysis))
#else
#define DFTRACER_UTILS_NO_THREAD_SAFETY_ANALYSIS
#endif

/// printf-style argument checking where the compiler has it, nothing on MSVC.
#if defined(__GNUC__) || defined(__clang__)
#define DFTRACER_UTILS_PRINTF_FORMAT(FMT_INDEX, ARG_INDEX) \
    __attribute__((__format__(__printf__, FMT_INDEX, ARG_INDEX)))
#else
#define DFTRACER_UTILS_PRINTF_FORMAT(FMT_INDEX, ARG_INDEX)
#endif

/// Tells the CPU that this thread spins, so a contended cache line settles.
#if defined(__x86_64__) || defined(__i386__)
#define DFTRACER_UTILS_CPU_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define DFTRACER_UTILS_CPU_PAUSE() __asm__ __volatile__("yield")
#else
#define DFTRACER_UTILS_CPU_PAUSE() ((void)0)
#endif

/// As a default argument, the file and line of the caller. Unlike the consteval
/// std::source_location::current, GCC 13 and later accept them in a
/// coroutine's get_return_object.
#define DFTRACER_UTILS_CALLER_FILE() __builtin_FILE()
#define DFTRACER_UTILS_CALLER_LINE() __builtin_LINE()

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define DFTRACER_UTILS_HAS_CPUID 1
#else
#define DFTRACER_UTILS_HAS_CPUID 0
#endif

namespace dftracer::utils::common {

/// Runs CPUID `leaf` and stores EAX, EBX, ECX and EDX in `regs`. False, with
/// `regs` untouched, on a CPU that is not x86-64 or when `leaf` is above the
/// highest leaf the CPU supports.
inline bool cpuid_leaf(std::uint32_t leaf, std::uint32_t (&regs)[4]) {
#if DFTRACER_UTILS_HAS_CPUID
    std::uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0u), "c"(0u));
    if (leaf > eax) return false;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(leaf), "c"(0u));
    regs[0] = eax;
    regs[1] = ebx;
    regs[2] = ecx;
    regs[3] = edx;
    return true;
#else
    (void)leaf;
    (void)regs;
    return false;
#endif
}

}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_PLATFORM_COMPAT_H
