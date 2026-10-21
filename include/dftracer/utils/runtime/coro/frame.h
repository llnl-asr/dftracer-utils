#ifndef DFTRACER_UTILS_RUNTIME_CORO_FRAME_H
#define DFTRACER_UTILS_RUNTIME_CORO_FRAME_H

#include <dftracer/utils/memory/heap.h>

#include <cstddef>

namespace dftracer::utils::coro {

inline constexpr std::size_t FRAME_ALIGN = __STDCPP_DEFAULT_NEW_ALIGNMENT__;

/// Base of a promise type: its frames come from the page heap and are charged
/// to the pool current at the call that creates the coroutine. With the
/// memory debug mode on, a destroyed frame is poisoned and not reused (see
/// memory::deallocate_retained).
struct FrameAllocation {
    static void* operator new(std::size_t size) {
        return memory::allocate(size, FRAME_ALIGN);
    }
    static void operator delete(void* ptr, std::size_t size) noexcept {
        memory::deallocate_retained(ptr, size);
    }
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_RUNTIME_CORO_FRAME_H
