#ifndef DFTRACER_UTILS_MEMORY_VG_H
#define DFTRACER_UTILS_MEMORY_VG_H

#include <cstddef>

#if defined(__has_include)
#if __has_include(<valgrind/memcheck.h>)
#define DFTRACER_UTILS_MEMORY_VALGRIND 1
#include <valgrind/memcheck.h>
#endif
#endif

namespace dftracer::utils::memory {

#ifdef DFTRACER_UTILS_MEMORY_VALGRIND

inline bool g_vg_active = false;

/// Read once by the heap before it hands out a block. Outside Valgrind every
/// request below is skipped behind one predictable branch.
inline void vg_init() noexcept { g_vg_active = RUNNING_ON_VALGRIND != 0; }

inline bool vg_active() noexcept { return g_vg_active; }

/// A block of @p size bytes inside a run of @p usable bytes. The slack stays
/// unaddressable, so an overrun past @p size is reported.
inline void vg_alloc(void* p, std::size_t size, std::size_t usable) noexcept {
    if (!vg_active()) [[likely]]
        return;
    VALGRIND_MALLOCLIKE_BLOCK(p, size, 0, 0);
    VALGRIND_MAKE_MEM_NOACCESS(static_cast<char*>(p) + size, usable - size);
}

/// Ends the block that vg_alloc started and makes all @p usable bytes
/// unaddressable.
inline void vg_free(void* p, std::size_t usable) noexcept {
    if (!vg_active()) [[likely]]
        return;
    VALGRIND_FREELIKE_BLOCK(p, 0);
    VALGRIND_MAKE_MEM_NOACCESS(p, usable);
}

/// The block keeps its address; bytes past @p new_size become unaddressable.
inline void vg_resize(void* p, std::size_t old_size,
                      std::size_t new_size) noexcept {
    if (!vg_active()) [[likely]]
        return;
    // Memcheck reports a resize in place to zero bytes as a bad free.
    if (new_size == 0) {
        VALGRIND_FREELIKE_BLOCK(p, 0);
        VALGRIND_MALLOCLIKE_BLOCK(p, 0, 0, 0);
        VALGRIND_MAKE_MEM_NOACCESS(p, old_size);
        return;
    }
    VALGRIND_RESIZEINPLACE_BLOCK(p, old_size, new_size, 0);
}

inline void vg_noaccess(void* p, std::size_t n) noexcept {
    if (vg_active()) [[unlikely]]
        VALGRIND_MAKE_MEM_NOACCESS(p, n);
}

/// Heap metadata that sits in pages the heap otherwise keeps unaddressable.
inline void vg_define(void* p, std::size_t n) noexcept {
    if (vg_active()) [[unlikely]]
        VALGRIND_MAKE_MEM_DEFINED(p, n);
}

#else

inline void vg_init() noexcept {}
constexpr bool vg_active() noexcept { return false; }
inline void vg_alloc(void*, std::size_t, std::size_t) noexcept {}
inline void vg_free(void*, std::size_t) noexcept {}
inline void vg_resize(void*, std::size_t, std::size_t) noexcept {}
inline void vg_noaccess(void*, std::size_t) noexcept {}
inline void vg_define(void*, std::size_t) noexcept {}

#endif

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_VG_H
