// SPDX-License-Identifier: MIT
// Replaces the global operator new and delete with this heap. Link it into
// executables only, since a shared library would interpose for the whole host
// process.
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>

#include <cstddef>
#include <new>

namespace {

namespace memory = dftracer::utils::memory;
using memory::Pool;
using memory::PoolHandle;

constexpr std::size_t DEFAULT_ALIGN = alignof(std::max_align_t);

// Blocks of the global operator new have a pool of their own, left out of the
// exit check because the C++ runtime keeps singletons that are never freed.
Pool& new_pool() {
    alignas(PoolHandle) static unsigned char storage[sizeof(PoolHandle)];
    static Pool* pool = [] {
        auto* handle =
            new (storage) PoolHandle(memory::Manager::get().root().child(
                "global_new", memory::PoolKind::Results));
        memory::exempt_from_exit_check(**handle);
        return handle->get();
    }();
    return *pool;
}

[[gnu::always_inline]] inline void* get(std::size_t size, std::size_t align,
                                        void* site) {
    return memory::allocate_at(size, align, new_pool(), site);
}

[[gnu::always_inline]] inline void* try_get(std::size_t size, std::size_t align,
                                            void* site) noexcept {
    try {
        return get(size, align, site);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

// Runs after the shared libraries are initialized and before the executable's
// static constructors, so the exit check runs before the libraries' static
// destructors and after the executable's.
[[gnu::constructor(101)]] void init() {
    (void)new_pool();
    memory::rearm_exit_check();
}

}  // namespace

void* operator new(std::size_t size) {
    return get(size, DEFAULT_ALIGN, __builtin_return_address(0));
}
void* operator new[](std::size_t size) {
    return get(size, DEFAULT_ALIGN, __builtin_return_address(0));
}
void* operator new(std::size_t size, std::align_val_t align) {
    return get(size, static_cast<std::size_t>(align),
               __builtin_return_address(0));
}
void* operator new[](std::size_t size, std::align_val_t align) {
    return get(size, static_cast<std::size_t>(align),
               __builtin_return_address(0));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return try_get(size, DEFAULT_ALIGN, __builtin_return_address(0));
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return try_get(size, DEFAULT_ALIGN, __builtin_return_address(0));
}
void* operator new(std::size_t size, std::align_val_t align,
                   const std::nothrow_t&) noexcept {
    return try_get(size, static_cast<std::size_t>(align),
                   __builtin_return_address(0));
}
void* operator new[](std::size_t size, std::align_val_t align,
                     const std::nothrow_t&) noexcept {
    return try_get(size, static_cast<std::size_t>(align),
                   __builtin_return_address(0));
}

void operator delete(void* p) noexcept { memory::deallocate(p); }
void operator delete[](void* p) noexcept { memory::deallocate(p); }
void operator delete(void* p, std::size_t) noexcept { memory::deallocate(p); }
void operator delete[](void* p, std::size_t) noexcept { memory::deallocate(p); }
void operator delete(void* p, std::align_val_t) noexcept {
    memory::deallocate(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    memory::deallocate(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    memory::deallocate(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    memory::deallocate(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    memory::deallocate(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    memory::deallocate(p);
}
void operator delete(void* p, std::align_val_t,
                     const std::nothrow_t&) noexcept {
    memory::deallocate(p);
}
void operator delete[](void* p, std::align_val_t,
                       const std::nothrow_t&) noexcept {
    memory::deallocate(p);
}
