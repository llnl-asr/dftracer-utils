#include <dftracer/utils/memory/attrib.h>
#include <dftracer/utils/memory/heap_system.h>
#include <dftracer/utils/memory/settings.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace dftracer::utils::memory {

void abort_with(const char* message) noexcept {
    std::size_t left = std::strlen(message);
    while (left > 0) {
        ssize_t n = ::write(STDERR_FILENO, message, left);
        if (n <= 0) break;
        message += n;
        left -= static_cast<std::size_t>(n);
    }
    std::abort();
}

namespace {

// One page per thread that allocates or frees through the system backend. A
// cache is never unmapped; the next thread that starts takes one that an
// exited thread released.
struct SystemCache {
    AttribCache attrib;
    std::atomic<SystemCache*> list_next{nullptr};
    std::atomic<bool> in_use{true};
};

thread_local SystemCache* t_cache = nullptr;
std::atomic<SystemCache*> g_system_caches{nullptr};
pthread_key_t g_system_key;
pthread_once_t g_system_once = PTHREAD_ONCE_INIT;

void release_cache(void* arg) {
    auto* c = static_cast<SystemCache*>(arg);
    attrib_set_scope(c->attrib, GENERAL_POOL_ID);
    t_cache = nullptr;
    c->in_use.store(false, std::memory_order_release);
}

void system_init() {
    if (::pthread_key_create(&g_system_key, release_cache) != 0) {
        abort_with("dftracer-utils memory: cannot register thread hooks\n");
    }
}

SystemCache* new_cache() {
    auto bytes = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    void* raw = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    if (raw == MAP_FAILED) throw std::bad_alloc();
    auto* c = new (raw) SystemCache{};
    attrib_register(c->attrib);
    SystemCache* head = g_system_caches.load(std::memory_order_relaxed);
    do {
        c->list_next.store(head, std::memory_order_relaxed);
    } while (!g_system_caches.compare_exchange_weak(
        head, c, std::memory_order_release, std::memory_order_relaxed));
    return c;
}

SystemCache* take_cache() {
    for (SystemCache* c = g_system_caches.load(std::memory_order_acquire);
         c != nullptr; c = c->list_next.load(std::memory_order_relaxed)) {
        if (!c->in_use.exchange(true, std::memory_order_acquire)) return c;
    }
    return new_cache();
}

AttribCache& attach_cache() {
    ::pthread_once(&g_system_once, system_init);
    SystemCache* c = take_cache();
    t_cache = c;
    if (::pthread_setspecific(g_system_key, c) != 0) {
        abort_with("dftracer-utils memory: cannot register thread cache\n");
    }
    return c->attrib;
}

}  // namespace

AttribCache& system_attrib() {
    if (t_cache == nullptr) [[unlikely]]
        return attach_cache();
    return t_cache->attrib;
}

void system_attrib_after_fork() noexcept {
    SystemCache* own = t_cache;
    for (SystemCache* c = g_system_caches.load(std::memory_order_acquire);
         c != nullptr; c = c->list_next.load(std::memory_order_relaxed)) {
        if (c != own) c->in_use.store(false, std::memory_order_relaxed);
    }
}

// posix_memalign, not aligned_alloc: aligned_alloc rounds the size up to the
// alignment, and the slack would hide an overrun from ASan.
void* system_allocate(std::size_t size, std::size_t align) {
    if (align == 0 || (align & (align - 1)) != 0) {
        abort_with("dftracer-utils memory: alignment is not a power of two\n");
    }
    std::size_t offset =
        align > SYSTEM_HEADER_SIZE ? align : SYSTEM_HEADER_SIZE;
    if (offset > std::numeric_limits<std::uint32_t>::max() ||
        size > std::numeric_limits<std::size_t>::max() - offset) {
        throw std::bad_alloc();
    }
    if (size > static_cast<std::size_t>(ATTRIB_FLUSH_BYTES)) {
        std::size_t cap = hard_cap();
        if (cap != 0 && static_cast<std::size_t>(
                            std::max<std::ptrdiff_t>(attrib_live_total(), 0)) +
                                size >
                            cap) {
            throw std::bad_alloc();
        }
    }
    void* base = nullptr;
    if (::posix_memalign(&base, offset, offset + size) != 0) {
        throw std::bad_alloc();
    }
    void* p = static_cast<char*>(base) + offset;
    AttribCache& attrib = system_attrib();
    std::uint16_t pool = attrib.scope;
    *system_header(p) =
        SystemHeader{static_cast<std::uint64_t>(size),
                     static_cast<std::uint32_t>(offset), pool, 0};
    attrib_charge_any(attrib, size);
    return p;
}

void system_deallocate(void* p) noexcept {
    if (p == nullptr) return;
    SystemHeader* header = system_header(p);
    attrib_credit_any(system_attrib(), header->pool_id, header->size);
    std::free(static_cast<char*>(p) - header->offset);
}

void system_deallocate(void* p, std::size_t size) noexcept {
    if (p == nullptr) return;
#ifdef DFTRACER_UTILS_MEMORY_CHECKS
    if (system_header(p)->size != size) {
        abort_with(
            "dftracer-utils memory: sized free does not match the allocated "
            "size\n");
    }
#else
    (void)size;
#endif
    system_deallocate(p);
}

// A C library block has no size class to grow into, and a block resized in
// place would hide an overrun past the new size from ASan.
void* system_reallocate(void* p, std::size_t old_size, std::size_t new_size,
                        std::size_t align) {
    if (align == 0 || (align & (align - 1)) != 0) {
        abort_with("dftracer-utils memory: alignment is not a power of two\n");
    }
    if (p == nullptr) return system_allocate(new_size, align);
    SystemHeader* header = system_header(p);
#ifdef DFTRACER_UTILS_MEMORY_CHECKS
    if (header->size != old_size) {
        abort_with(
            "dftracer-utils memory: sized free does not match the allocated "
            "size\n");
    }
#endif
    if (new_size == header->size &&
        (reinterpret_cast<std::uintptr_t>(p) & (align - 1)) == 0) {
        return p;
    }
    void* moved;
    {
        ScopeOverride scope{system_attrib(), header->pool_id};
        moved = system_allocate(new_size, align);
    }
    std::memcpy(moved, p, std::min(old_size, new_size));
    system_deallocate(p);
    return moved;
}

}  // namespace dftracer::utils::memory
