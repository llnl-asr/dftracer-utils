#ifndef DFTRACER_UTILS_CORE_COMMON_FILESYSTEM_H
#define DFTRACER_UTILS_CORE_COMMON_FILESYSTEM_H

#if defined(DFTRACER_UTILS_HAS_STD_FILESYSTEM)

#include <filesystem>

namespace fs = std::filesystem;

#else

// C++11 compatible filesystem header
// This header provides a portable way to use filesystem across different C++
// standards Based on gulrak/filesystem detection logic with additional support
// for std::__fs::filesystem

// First, determine if we can use std::filesystem
#if (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || \
    (__cplusplus >= 201703L && defined(__has_include))
// ^ Supports MSVC prior to 15.7 without setting /Zc:__cplusplus to fix
// __cplusplus _MSVC_LANG works regardless. But without the switch, the compiler
// always reported 199711L
#if __has_include( \
    <filesystem>)  // Two stage __has_include needed for MSVC 2015 and per GCC docs
#define DFTRACE_UTILS_USE_STD_FS

// Old Apple OSs don't support std::filesystem, though the header is available
// at compile time. In particular, std::filesystem is unavailable before
// macOS 10.15, iOS/tvOS 13.0, and watchOS 6.0.
#ifdef __APPLE__
#include <Availability.h>
// Note: This intentionally uses std::filesystem on any new Apple OS, like
// visionOS released after std::filesystem, where std::filesystem is always
// available. (All other __<platform>_VERSION_MIN_REQUIREDs will be undefined
// and thus 0.)
#if __MAC_OS_X_VERSION_MIN_REQUIRED &&                                       \
        __MAC_OS_X_VERSION_MIN_REQUIRED < 101500 ||                          \
    __IPHONE_OS_VERSION_MIN_REQUIRED &&                                      \
        __IPHONE_OS_VERSION_MIN_REQUIRED < 130000 ||                         \
    __TV_OS_VERSION_MIN_REQUIRED && __TV_OS_VERSION_MIN_REQUIRED < 130000 || \
    __WATCH_OS_VERSION_MAX_ALLOWED && __WATCH_OS_VERSION_MAX_ALLOWED < 60000
#undef DFTRACE_UTILS_USE_STD_FS
#endif
#endif
#endif
#endif

// check for std::__fs::filesystem (Clang with C++11/14, but not on Apple
// platforms)
#if !defined(DFTRACE_UTILS_USE_STD_FS) && defined(__has_include) && \
    !defined(__APPLE__)
#if __has_include(<filesystem>)
// check if std::__fs::filesystem exists (common in Clang with C++11/14)
#if defined(__clang__) && __clang_major__ >= 7
#define DFTRACE_UTILS_USE_STD_FS_INTERNAL
#endif
#endif
#endif

// try experimental filesystem (but only if we have a good reason to avoid
// ghc_filesystem)
#if !defined(DFTRACE_UTILS_USE_STD_FS) && \
    !defined(DFTRACE_UTILS_USE_STD_FS_INTERNAL) && defined(__has_include) && 0
#if __has_include(<experimental/filesystem>)
#define DFTRACE_UTILS_USE_EXPERIMENTAL_FS
#endif
#endif

// include the appropriate headers and set up the namespace
#ifdef DFTRACE_UTILS_USE_STD_FS
#include <filesystem>
namespace fs = std::filesystem;
#elif defined(DFTRACE_UTILS_USE_STD_FS_INTERNAL)
#include <filesystem>
namespace fs = std::__fs::filesystem;
#elif defined(DFTRACE_UTILS_USE_EXPERIMENTAL_FS)
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
// fallback to gulrak/filesystem for C++11 compatibility
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;

#endif  // DFTRACE_UTILS_USE_STD_FS

#endif

#endif  // DFTRACER_UTILS_CORE_COMMON_FILESYSTEM_H
