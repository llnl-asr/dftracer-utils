// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_EXPORT_H
#define DFTRACER_UTILS_COMMON_EXPORT_H

/*
 * Symbol visibility for the dftracer-utils stable C ABIs. Freestanding - pure
 * preprocessor, no includes - so even a self-contained plugin ABI can use it.
 *
 *   DFTU_EXPORT         mark a stable-ABI symbol default-visible, so it stays
 *                      exported even when the library hides symbols by default.
 *   DFTU_LOCAL          force a symbol hidden (never exported).
 *   DFTU_PLUGIN_EXPORT  a plugin's exported entry point (same effect as
 *                      DFTU_EXPORT, named for intent at the plugin edge).
 *
 * Windows uses a single dllexport (no dllimport split) pending Windows support.
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#define DFTU_EXPORT __declspec(dllexport)
#define DFTU_LOCAL
#elif defined(__GNUC__) || defined(__clang__)
#define DFTU_EXPORT __attribute__((visibility("default")))
#define DFTU_LOCAL __attribute__((visibility("hidden")))
#else
#define DFTU_EXPORT
#define DFTU_LOCAL
#endif

#define DFTU_PLUGIN_EXPORT DFTU_EXPORT

#if defined(_MSC_VER)
#define DFTRACER_UTILS_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define DFTRACER_UTILS_NOINLINE __attribute__((noinline))
#else
#define DFTRACER_UTILS_NOINLINE
#endif

/// For a small function on a hot path whose callers the compiler would not
/// inline it into. Use with an `inline` function.
#if defined(_MSC_VER)
#define DFTRACER_UTILS_ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define DFTRACER_UTILS_ALWAYS_INLINE __attribute__((always_inline))
#else
#define DFTRACER_UTILS_ALWAYS_INLINE
#endif

#endif  // DFTRACER_UTILS_COMMON_EXPORT_H
