#ifndef DFTRACER_UTILS_MEMORY_ASAN_H
#define DFTRACER_UTILS_MEMORY_ASAN_H

#include <cstddef>

#if defined(__SANITIZE_ADDRESS__)
#define DFTRACER_UTILS_MEMORY_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DFTRACER_UTILS_MEMORY_ASAN 1
#endif
#endif

#ifdef DFTRACER_UTILS_MEMORY_ASAN
#include <sanitizer/asan_interface.h>
#endif

namespace dftracer::utils::memory {

#ifdef DFTRACER_UTILS_MEMORY_ASAN

/// Mark @p n bytes as not addressable. A start or length that is not a
/// multiple of 8 leaves the granule it cuts accessible.
inline void asan_poison(const void* p, std::size_t n) noexcept {
    ASAN_POISON_MEMORY_REGION(p, n);
}

/// Make @p n bytes addressable. The last granule becomes partly addressable
/// when @p n is not a multiple of 8, so a write one byte past is reported.
inline void asan_unpoison(const void* p, std::size_t n) noexcept {
    ASAN_UNPOISON_MEMORY_REGION(p, n);
}

#else

inline void asan_poison(const void*, std::size_t) noexcept {}
inline void asan_unpoison(const void*, std::size_t) noexcept {}

#endif

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_ASAN_H
