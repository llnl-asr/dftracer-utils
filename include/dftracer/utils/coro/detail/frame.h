// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_DETAIL_FRAME_H
#define DFTRACER_UTILS_CORO_DETAIL_FRAME_H

#include <dftracer/utils/memory/detail/heap.h>

#include <cstddef>

namespace dftracer::utils::coro::detail {

inline constexpr std::size_t FRAME_ALIGN = __STDCPP_DEFAULT_NEW_ALIGNMENT__;

/// Base of a promise type: its frames come from the page heap and are charged
/// to the pool current at the call that creates the coroutine. With the
/// memory debug mode on, a destroyed frame is poisoned and not reused (see
/// memory::detail::deallocate_retained).
struct FrameAllocation {
    static void* operator new(std::size_t size) {
        return memory::detail::allocate(size, FRAME_ALIGN);
    }
    static void operator delete(void* ptr, std::size_t size) noexcept {
        memory::detail::deallocate_retained(ptr, size);
    }
};

}  // namespace dftracer::utils::coro::detail

#endif  // DFTRACER_UTILS_CORO_DETAIL_FRAME_H
