#include <dftracer/utils/memory/attrib.h>
#include <dftracer/utils/memory/debug.h>
#include <dftracer/utils/memory/heap_page.h>
#include <dftracer/utils/memory/heap_system.h>
#include <dftracer/utils/memory/settings.h>
#include <sys/mman.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

namespace dftracer::utils::memory {

namespace {

constexpr std::uint32_t STATE_FREED = 0;
constexpr std::uint32_t STATE_LIVE = 1;

constexpr std::size_t TABLE_MASK = DEBUG_TABLE_SLOTS - 1;
constexpr std::size_t GROUP_SLOTS = DEBUG_SITE_GROUPS * 2;
constexpr std::size_t GROUP_MASK = GROUP_SLOTS - 1;

// A slot keeps its address for good: a freed block stays in the table so a
// second free finds it, and the next block at that address reuses the slot.
// The mapping is zero-filled, which is the initial state of every field.
struct alignas(32) Slot {
    std::atomic<std::uintptr_t> key;
    std::atomic<std::uintptr_t> site;
    std::atomic<std::size_t> size;
    std::atomic<std::uint32_t> state;
    std::atomic<std::uint16_t> pool;
};

struct Group {
    std::uintptr_t site;
    std::size_t blocks;
    std::size_t bytes;
    std::uint16_t pool;
    bool used;
};

std::atomic<Slot*> g_table{nullptr};
std::atomic<std::uintptr_t> g_quarantine[DEBUG_QUARANTINE_BLOCKS];
std::atomic<std::uint32_t> g_cursor{0};

std::size_t hash_of(std::uintptr_t key, std::uint16_t extra = 0) noexcept {
    std::uint64_t h = (key >> 4) ^ (std::uint64_t{extra} << 48);
    h *= 0x9e3779b97f4a7c15ull;
    return static_cast<std::size_t>(h >> 24);
}

[[noreturn]] void die(const char* what, const void* p, std::size_t size,
                      std::uintptr_t site) noexcept {
    char message[256];
    std::snprintf(message, sizeof(message),
                  "dftracer-utils memory: debug: %s: block %p of %zu bytes "
                  "allocated at site 0x%llx\n",
                  what, p, size, static_cast<unsigned long long>(site));
    abort_with(message);
}

Slot* table() noexcept {
    Slot* t = g_table.load(std::memory_order_acquire);
    if (t != nullptr) [[likely]]
        return t;
    constexpr std::size_t bytes = DEBUG_TABLE_SLOTS * sizeof(Slot);
    void* raw = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) {
        abort_with("dftracer-utils memory: cannot map the debug table\n");
    }
    Slot* expected = nullptr;
    if (!g_table.compare_exchange_strong(expected, static_cast<Slot*>(raw),
                                         std::memory_order_acq_rel)) {
        ::munmap(raw, bytes);
        return expected;
    }
    return static_cast<Slot*>(raw);
}

Slot* lookup(const void* p, bool insert) noexcept {
    auto key = reinterpret_cast<std::uintptr_t>(p);
    Slot* t = table();
    std::size_t i = hash_of(key) & TABLE_MASK;
    for (std::size_t n = 0; n < DEBUG_TABLE_SLOTS;
         ++n, i = (i + 1) & TABLE_MASK) {
        std::uintptr_t k = t[i].key.load(std::memory_order_acquire);
        if (k == key) return &t[i];
        if (k != 0) continue;
        if (!insert) return nullptr;
        std::uintptr_t expected = 0;
        if (t[i].key.compare_exchange_strong(expected, key,
                                             std::memory_order_acq_rel) ||
            expected == key) {
            return &t[i];
        }
    }
    if (insert) abort_with("dftracer-utils memory: the debug table is full\n");
    return nullptr;
}

void* base_allocate(std::size_t size, std::size_t align) {
    return active_backend() == Backend::PageHeap ? page_allocate(size, align)
                                                 : system_allocate(size, align);
}

void base_deallocate(void* p) noexcept {
    if (active_backend() == Backend::PageHeap) {
        page_deallocate(p);
    } else {
        system_deallocate(p);
    }
}

void release(void* p) noexcept {
    Slot* s = lookup(p, false);
    std::size_t size = s->size.load(std::memory_order_relaxed);
    const auto* bytes = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < size + DEBUG_REDZONE_BYTES; ++i) {
        if (bytes[i] != DEBUG_FREED_FILL) {
            die("a freed block was written", p, size,
                s->site.load(std::memory_order_relaxed));
        }
    }
    base_deallocate(p);
}

}  // namespace

void* debug_allocate(std::size_t size, std::size_t align, void* site) {
    if (size > std::numeric_limits<std::size_t>::max() - DEBUG_REDZONE_BYTES) {
        throw std::bad_alloc();
    }
    void* p = base_allocate(size + DEBUG_REDZONE_BYTES, align);
    Slot* s = lookup(p, true);
    std::uint32_t expected = STATE_FREED;
    if (!s->state.compare_exchange_strong(expected, STATE_LIVE,
                                          std::memory_order_acq_rel)) {
        die("the backend returned a live block", p, size,
            s->site.load(std::memory_order_relaxed));
    }
    s->size.store(size, std::memory_order_relaxed);
    s->site.store(reinterpret_cast<std::uintptr_t>(site),
                  std::memory_order_relaxed);
    s->pool.store(attrib_local().scope, std::memory_order_relaxed);
    std::memset(static_cast<char*>(p) + size, DEBUG_REDZONE_FILL,
                DEBUG_REDZONE_BYTES);
    return p;
}

void debug_deallocate(void* p, std::size_t size, bool sized) noexcept {
    if (p == nullptr) return;
    Slot* s = lookup(p, false);
    if (s == nullptr) {
        die("free of a pointer this library did not allocate", p, 0, 0);
    }
    std::uint32_t expected = STATE_LIVE;
    if (!s->state.compare_exchange_strong(expected, STATE_FREED,
                                          std::memory_order_acq_rel)) {
        die("double free", p, s->size.load(std::memory_order_relaxed),
            s->site.load(std::memory_order_relaxed));
    }
    std::size_t have = s->size.load(std::memory_order_relaxed);
    std::uintptr_t site = s->site.load(std::memory_order_relaxed);
    if (sized && size != have) {
        die("sized free does not match the allocated size", p, have, site);
    }
    auto* bytes = static_cast<unsigned char*>(p);
    for (std::size_t i = 0; i < DEBUG_REDZONE_BYTES; ++i) {
        if (bytes[have + i] != DEBUG_REDZONE_FILL) {
            die("the redzone behind the block was written", p, have, site);
        }
    }
    std::memset(p, DEBUG_FREED_FILL, have + DEBUG_REDZONE_BYTES);
    std::uint32_t slot = g_cursor.fetch_add(1, std::memory_order_relaxed) %
                         DEBUG_QUARANTINE_BLOCKS;
    auto old = g_quarantine[slot].exchange(reinterpret_cast<std::uintptr_t>(p),
                                           std::memory_order_acq_rel);
    if (old != 0) release(reinterpret_cast<void*>(old));
}

void debug_drain() noexcept {
    if (g_table.load(std::memory_order_acquire) == nullptr) return;
    for (auto& entry : g_quarantine) {
        auto old = entry.exchange(0, std::memory_order_acq_rel);
        if (old != 0) release(reinterpret_cast<void*>(old));
    }
}

void debug_for_each_site(std::uint16_t pool, DebugVisit visit,
                         void* context) noexcept {
    Slot* t = g_table.load(std::memory_order_acquire);
    if (t == nullptr) return;
    constexpr std::size_t bytes = GROUP_SLOTS * sizeof(Group);
    void* raw = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    if (raw == MAP_FAILED) {
        abort_with("dftracer-utils memory: cannot map the leak report\n");
    }
    auto* groups = static_cast<Group*>(raw);
    std::size_t count = 0;
    DebugSite other{DEBUG_ALL_POOLS, 0, 0, 0};
    for (std::size_t i = 0; i < DEBUG_TABLE_SLOTS; ++i) {
        if (t[i].state.load(std::memory_order_acquire) != STATE_LIVE) continue;
        std::uint16_t owner = t[i].pool.load(std::memory_order_relaxed);
        if (pool != DEBUG_ALL_POOLS && owner != pool) continue;
        std::uintptr_t site = t[i].site.load(std::memory_order_relaxed);
        std::size_t size = t[i].size.load(std::memory_order_relaxed);
        std::size_t g = hash_of(site, owner) & GROUP_MASK;
        while (groups[g].used &&
               (groups[g].site != site || groups[g].pool != owner)) {
            g = (g + 1) & GROUP_MASK;
        }
        if (!groups[g].used) {
            if (count == DEBUG_SITE_GROUPS) {
                ++other.blocks;
                other.bytes += size;
                continue;
            }
            groups[g] = Group{site, 0, 0, owner, true};
            ++count;
        }
        ++groups[g].blocks;
        groups[g].bytes += size;
    }
    for (std::size_t g = 0; g < GROUP_SLOTS; ++g) {
        if (!groups[g].used) continue;
        visit(context, DebugSite{groups[g].pool, groups[g].site,
                                 groups[g].blocks, groups[g].bytes});
    }
    if (other.blocks != 0) visit(context, other);
    ::munmap(raw, bytes);
}

}  // namespace dftracer::utils::memory
