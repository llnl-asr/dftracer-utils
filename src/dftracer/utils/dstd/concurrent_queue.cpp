// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/concurrent_queue.h>
#include <dftracer/utils/memory/detail/heap.h>
#include <dftracer/utils/memory/detail/manager.h>

#include <new>

namespace dftracer::utils::dstd {
namespace {

// Never closed, so the handle is built in place and never destroyed. The exit
// check still counts the pool, so a queue alive at exit is reported.
memory::Pool& queue_pool() {
    alignas(memory::PoolHandle) static unsigned char
        storage[sizeof(memory::PoolHandle)];
    static memory::Pool* pool = [] {
        auto* handle = new (storage)
            memory::PoolHandle(memory::detail::Manager::get().root().child(
                "concurrent_queue", memory::PoolKind::Results));
        return handle->get();
    }();
    return *pool;
}

}  // namespace

void* ConcurrentQueueTraits::malloc(std::size_t size) noexcept {
    return memory::detail::try_allocate_at(size, alignof(std::max_align_t),
                                           queue_pool(),
                                           __builtin_return_address(0));
}

void ConcurrentQueueTraits::free(void* p) noexcept {
    memory::detail::deallocate(p);
}

}  // namespace dftracer::utils::dstd
