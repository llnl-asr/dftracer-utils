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

// Memory alignment constants for optimal performance
#ifdef __APPLE__
// Apple Silicon (M1/M2/M3) has 128-byte cache lines for optimal performance
#define DFTRACER_CACHE_LINE_SIZE 128
#define DFTRACER_OPTIMAL_ALIGNMENT 128
#elif defined(__x86_64__) || defined(_M_X64)
// x86_64 systems typically have 64-byte cache lines
#define DFTRACER_CACHE_LINE_SIZE 64
#define DFTRACER_OPTIMAL_ALIGNMENT 64
#elif defined(__aarch64__) && !defined(__APPLE__)
// ARM64 Linux systems typically have 64-byte cache lines
#define DFTRACER_CACHE_LINE_SIZE 64
#define DFTRACER_OPTIMAL_ALIGNMENT 64
#else
// Conservative default for other architectures
#define DFTRACER_CACHE_LINE_SIZE 64
#define DFTRACER_OPTIMAL_ALIGNMENT 64
#endif

// Convenience macro for aligned buffer declarations
#define DFTRACER_ALIGNED_BUFFER(type, name, size) \
    alignas(DFTRACER_OPTIMAL_ALIGNMENT) type name[size]

#endif  // DFTRACER_UTILS_CORE_COMMON_PLATFORM_COMPAT_H
