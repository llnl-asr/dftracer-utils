// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/concurrent_queue.h>
#include <dftracer/utils/memory/detail/heap.h>
#include <dftracer/utils/memory/detail/manager.h>

#include <new>

namespace dftracer::utils::dstd {
namespace {

// Never closed, so the handle is built in place and never destroyed. The exit
// check still counts the pool, so a queue alive at exit is reported.
memory::detail::Pool& queue_pool() {
    alignas(memory::detail::PoolHandle) static unsigned char
        storage[sizeof(memory::detail::PoolHandle)];
    static memory::detail::Pool* pool = [] {
        auto* handle = new (storage) memory::detail::PoolHandle(
            memory::detail::Manager::get().root().child(
                "concurrent_queue", memory::detail::PoolKind::Results));
        return handle->get();
    }();
    return *pool;
}

}  // namespace

void* ConcurrentQueueTraits::malloc(std::size_t size) noexcept {
    try {
        return memory::detail::allocate(size, alignof(std::max_align_t),
                                        queue_pool());
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void ConcurrentQueueTraits::free(void* p) noexcept {
    memory::detail::deallocate(p);
}

}  // namespace dftracer::utils::dstd
