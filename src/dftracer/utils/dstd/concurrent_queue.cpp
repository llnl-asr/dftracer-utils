// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/concurrent_queue.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>

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
            memory::PoolHandle(memory::Manager::get().root().child(
                "concurrent_queue", memory::PoolKind::Results));
        return handle->get();
    }();
    return *pool;
}

}  // namespace

void* ConcurrentQueueTraits::malloc(std::size_t size) noexcept {
    try {
        return memory::allocate(size, alignof(std::max_align_t), queue_pool());
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void ConcurrentQueueTraits::free(void* p) noexcept { memory::deallocate(p); }

}  // namespace dftracer::utils::dstd
