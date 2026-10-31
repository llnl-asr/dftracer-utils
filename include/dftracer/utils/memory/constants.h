#ifndef DFTRACER_UTILS_MEMORY_CONSTANTS_H
#define DFTRACER_UTILS_MEMORY_CONSTANTS_H

#include <cstdint>

namespace dftracer::utils::memory::constants {
/// "page_heap" or "system", read once before the first allocation. When unset
/// or empty, the backend is the CMake option DFTRACER_UTILS_MEMORY_BACKEND
/// (page_heap unless set; the sanitizer presets set system). Any other value
/// aborts the process at the first allocation.
inline constexpr const char* MEMORY_BACKEND_ENV =
    "DFTRACER_UTILS_MEMORY_BACKEND";

/// Milliseconds an idle page, span or large run stays backed before a slow
/// path purges it, read once before the first allocation. Zero purges at the
/// first slow path. Anything that is not a whole number up to 4294967295
/// aborts the process at the first allocation.
inline constexpr const char* DECAY_MS_ENV = "DFTRACER_UTILS_MEM_DECAY_MS";
inline constexpr std::uint32_t DEFAULT_DECAY_MS = 1000;

/// The sum of reservations the root accepts, in bytes or with a unit such as
/// "4GB". Unset is zero, a third of the available memory.
inline constexpr const char* BUDGET_ENV = "DFTRACER_UTILS_MEM_BUDGET";
inline constexpr std::uint64_t DEFAULT_BUDGET_BYTES = 0;

/// Bytes of heap the process may hold from the operating system; an
/// allocation that would pass it throws. Zero is no cap.
inline constexpr const char* HARD_LIMIT_ENV = "DFTRACER_UTILS_MEM_HARD_LIMIT";
inline constexpr std::uint64_t DEFAULT_HARD_LIMIT_BYTES = 0;

/// How far below the process limit the measured memory may grow before
/// Manager::under_pressure turns on: a share of the limit, at least a floor.
inline constexpr const char* LIMIT_HEADROOM_ENV =
    "DFTRACER_UTILS_MEM_LIMIT_HEADROOM";
inline constexpr std::uint64_t LIMIT_HEADROOM_PERCENT = 5;
inline constexpr std::uint64_t LIMIT_HEADROOM_MIN_BYTES = std::uint64_t{256}
                                                          << 20;

/// How far past the budget a pool that is reclaiming may reserve: a share of
/// the budget, at least a floor.
inline constexpr const char* SPILL_HEADROOM_ENV =
    "DFTRACER_UTILS_MEM_SPILL_HEADROOM";
inline constexpr std::uint64_t SPILL_HEADROOM_PERCENT = 2;
inline constexpr std::uint64_t SPILL_HEADROOM_MIN_BYTES = std::uint64_t{32}
                                                          << 20;

/// "1" turns on the debug checks, read once before the first allocation: a
/// side table of the caller and size of each live block, a redzone behind each
/// block, a fill pattern on freed blocks and double free detection. Each
/// allocation is slower, and blocks stay counted by their pool until they leave
/// quarantine.
inline constexpr const char* DEBUG_ENV = "DFTRACER_UTILS_MEMORY_DEBUG";
inline constexpr bool DEFAULT_DEBUG = false;

/// "1" makes the process exit with status EXIT_CHECK_STATUS when blocks that
/// no host buffer owns are still live or a pool leaked at exit, read once
/// before the first allocation.
inline constexpr const char* EXIT_CHECK_ENV =
    "DFTRACER_UTILS_MEMORY_EXIT_CHECK";
inline constexpr bool DEFAULT_EXIT_CHECK = false;
inline constexpr int EXIT_CHECK_STATUS = 70;

/// Milliseconds between two reads of the process memory counters.
inline constexpr std::uint64_t PRESSURE_SAMPLE_MS = 200;
}  // namespace dftracer::utils::memory::constants

#endif  // DFTRACER_UTILS_MEMORY_CONSTANTS_H
