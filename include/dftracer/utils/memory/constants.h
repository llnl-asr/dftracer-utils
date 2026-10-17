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
}  // namespace dftracer::utils::memory::constants

#endif  // DFTRACER_UTILS_MEMORY_CONSTANTS_H
