#ifndef DFTRACER_UTILS_CORE_COMMON_PLATFORM_COMPAT_H
#define DFTRACER_UTILS_CORE_COMMON_PLATFORM_COMPAT_H

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
#define DFTRACER_CACHE_LINE_SIZE 256
#define DFTRACER_OPTIMAL_ALIGNMENT 256
#elif defined(__APPLE__) || defined(__ppc64__) || defined(__PPC64__) || \
    defined(_ARCH_PPC64)
// Apple Silicon: 128-byte cache lines
// IBM POWER9/10: 128-byte cache lines
#define DFTRACER_CACHE_LINE_SIZE 128
#define DFTRACER_OPTIMAL_ALIGNMENT 128
#elif defined(__x86_64__) || defined(_M_X64)
// Intel/AMD x86_64: 64-byte cache lines, 128-byte spatial prefetch pairs.
// Use 128 to avoid false sharing from adjacent-line prefetcher.
#define DFTRACER_CACHE_LINE_SIZE 64
#define DFTRACER_OPTIMAL_ALIGNMENT 128
#elif defined(__aarch64__)
// ARM64 Linux (Graviton, Neoverse, etc.): 64-byte cache lines
#define DFTRACER_CACHE_LINE_SIZE 64
#define DFTRACER_OPTIMAL_ALIGNMENT 64
#else
// Conservative default
#define DFTRACER_CACHE_LINE_SIZE 64
#define DFTRACER_OPTIMAL_ALIGNMENT 128
#endif

// Convenience macro for aligned buffer declarations
#define DFTRACER_ALIGNED_BUFFER(type, name, size) \
    alignas(DFTRACER_OPTIMAL_ALIGNMENT) type name[size]

// std::thread::hardware_concurrency() may return 0 on some platforms.
// Use this wrapper to guarantee at least 1 thread.
#include <cstddef>
#include <cstdlib>
#include <thread>

inline std::size_t dftracer_utils_hardware_concurrency() {
    if (const char *env = std::getenv("DFTRACER_UTILS_HW_CONCURRENCY")) {
        char *end = nullptr;
        unsigned long v = std::strtoul(env, &end, 10);
        if (end != env && v > 0) return static_cast<std::size_t>(v);
    }
    auto n = std::thread::hardware_concurrency();
    return n == 0 ? 1u : static_cast<std::size_t>(n);
}

// ThreadSanitizer annotations for custom synchronization primitives.
#if defined(__SANITIZE_THREAD__)
#define DFTRACER_TSAN_ENABLED 1
#endif
#if !defined(DFTRACER_TSAN_ENABLED) && defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DFTRACER_TSAN_ENABLED 1
#endif
#endif

#if defined(DFTRACER_TSAN_ENABLED)
#if __has_include(<sanitizer/tsan_interface.h>)
#include <sanitizer/tsan_interface.h>
#else
extern "C" void __tsan_acquire(void *);
extern "C" void __tsan_release(void *);
#endif
#define DFTRACER_TSAN_ACQUIRE(addr) __tsan_acquire(addr)
#define DFTRACER_TSAN_RELEASE(addr) __tsan_release(addr)
#else
#define DFTRACER_TSAN_ACQUIRE(addr) ((void)0)
#define DFTRACER_TSAN_RELEASE(addr) ((void)0)
#endif

#endif  // DFTRACER_UTILS_CORE_COMMON_PLATFORM_COMPAT_H
