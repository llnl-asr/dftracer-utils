#ifndef DFTRACER_UTILS_MEMORY_LINK_H
#define DFTRACER_UTILS_MEMORY_LINK_H

#include <dftracer/utils/memory/asan.h>
#include <dftracer/utils/memory/vg.h>

#include <cstddef>

namespace dftracer::utils::memory {

// The heap keeps the free-list link in the first bytes of a free block, which
// ASan has poisoned and Valgrind has made unaddressable. These two functions
// are the only code that reads or writes it, so they are the only ones that
// step around the checks.
#ifdef DFTRACER_UTILS_MEMORY_ASAN
#define DFTRACER_UTILS_MEMORY_LINK_ATTRS \
    [[gnu::noinline, gnu::no_sanitize("address")]]
#else
#define DFTRACER_UTILS_MEMORY_LINK_ATTRS
#endif

DFTRACER_UTILS_MEMORY_LINK_ATTRS inline void* load_link(void* block) noexcept {
    if (vg_active()) [[unlikely]] {
        vg_define(block, sizeof(void*));
        void* next = *static_cast<void**>(block);
        vg_noaccess(block, sizeof(void*));
        return next;
    }
    return *static_cast<void**>(block);
}

DFTRACER_UTILS_MEMORY_LINK_ATTRS inline void store_link(void* block,
                                                        void* next) noexcept {
    if (vg_active()) [[unlikely]] {
        vg_define(block, sizeof(void*));
        *static_cast<void**>(block) = next;
        vg_noaccess(block, sizeof(void*));
        return;
    }
    *static_cast<void**>(block) = next;
}

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_LINK_H
