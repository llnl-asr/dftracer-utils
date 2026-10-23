#include <dftracer/utils/memory/attrib.h>
#include <dftracer/utils/memory/debug.h>
#include <dftracer/utils/memory/heap_page.h>
#include <dftracer/utils/memory/heap_system.h>
#include <dftracer/utils/memory/settings.h>
#include <sys/mman.h>

#include <algorithm>
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

struct alignas(32) SiteTotal {
    std::atomic<std::uintptr_t> site;
    std::atomic<std::ptrdiff_t> live;
    std::atomic<std::size_t> peak;
    std::atomic<std::size_t> allocations;
};

std::atomic<SiteTotal*> g_totals{nullptr};
// Per-site live bytes at the last snapshot, indexed like the totals.
std::atomic<std::ptrdiff_t>* g_at_peak = nullptr;
std::atomic<std::ptrdiff_t> g_live_total{0};
std::atomic<std::ptrdiff_t> g_live_big{0};
std::atomic<std::size_t> g_snapshot_floor{DEBUG_PEAK_MIN_STEP};
std::atomic<bool> g_snapshotting{false};
std::atomic<std::size_t> g_peak_total{0};
std::atomic<std::size_t> g_peak_big{0};
std::atomic<Slot*> g_table{nullptr};
std::atomic<std::uintptr_t> g_quarantine[DEBUG_QUARANTINE_BLOCKS];
std::atomic<std::uint32_t> g_cursor{0};
std::atomic<std::uint64_t> g_allocations{0};
std::atomic<std::atomic<std::uintptr_t>*> g_held{nullptr};
std::atomic<std::size_t> g_held_count{0};

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

SiteTotal* totals() noexcept {
    SiteTotal* t = g_totals.load(std::memory_order_acquire);
    if (t != nullptr) [[likely]]
        return t;
    constexpr std::size_t bytes = DEBUG_SITE_TOTALS * sizeof(SiteTotal);
    void* raw = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) {
        abort_with("dftracer-utils memory: cannot map the site totals\n");
    }
    SiteTotal* expected = nullptr;
    if (!g_totals.compare_exchange_strong(expected,
                                          static_cast<SiteTotal*>(raw),
                                          std::memory_order_acq_rel)) {
        ::munmap(raw, bytes);
        return expected;
    }
    return static_cast<SiteTotal*>(raw);
}

// Null when the table is full; that caller is then left out of the totals.
SiteTotal* total_of(std::uintptr_t site) noexcept {
    if (site == 0) site = 1;
    SiteTotal* t = totals();
    std::size_t i = hash_of(site) & (DEBUG_SITE_TOTALS - 1);
    for (std::size_t n = 0; n < DEBUG_SITE_TOTALS;
         ++n, i = (i + 1) & (DEBUG_SITE_TOTALS - 1)) {
        std::uintptr_t k = t[i].site.load(std::memory_order_acquire);
        if (k == site) return &t[i];
        if (k != 0) continue;
        std::uintptr_t expected = 0;
        if (t[i].site.compare_exchange_strong(expected, site,
                                              std::memory_order_acq_rel) ||
            expected == site) {
            return &t[i];
        }
    }
    return nullptr;
}

// Copies the per-site live bytes when the total passed the floor. Takes one
// pass over the totals, and a thread that finds another one running leaves it.
[[gnu::noinline]] void snapshot(std::size_t total) noexcept {
    if (g_snapshotting.exchange(true, std::memory_order_acquire)) return;
    if (total >= g_snapshot_floor.load(std::memory_order_relaxed)) {
        SiteTotal* t = totals();
        if (g_at_peak == nullptr) {
            constexpr std::size_t bytes =
                DEBUG_SITE_TOTALS * sizeof(std::atomic<std::ptrdiff_t>);
            void* raw = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
            if (raw == MAP_FAILED) {
                abort_with("dftracer-utils memory: cannot map the peak\n");
            }
            g_at_peak = static_cast<std::atomic<std::ptrdiff_t>*>(raw);
        }
        for (std::size_t i = 0; i < DEBUG_SITE_TOTALS; ++i) {
            if (t[i].site.load(std::memory_order_acquire) == 0) continue;
            g_at_peak[i].store(t[i].live.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
        }
        auto big = g_live_big.load(std::memory_order_relaxed);
        g_peak_big.store(big > 0 ? static_cast<std::size_t>(big) : 0,
                         std::memory_order_relaxed);
        g_peak_total.store(total, std::memory_order_release);
        g_snapshot_floor.store(
            total +
                std::max(total / 100 * DEBUG_PEAK_PERCENT, DEBUG_PEAK_MIN_STEP),
            std::memory_order_relaxed);
    }
    g_snapshotting.store(false, std::memory_order_release);
}

void count_allocation(std::uintptr_t site, std::size_t size) noexcept {
    SiteTotal* t = total_of(site);
    if (t == nullptr) return;
    t->allocations.fetch_add(1, std::memory_order_relaxed);
    if (size > DEBUG_BIG_BLOCK_BYTES) {
        g_live_big.fetch_add(static_cast<std::ptrdiff_t>(size),
                             std::memory_order_relaxed);
    }
    auto live = t->live.fetch_add(static_cast<std::ptrdiff_t>(size),
                                  std::memory_order_relaxed) +
                static_cast<std::ptrdiff_t>(size);
    std::size_t peak = t->peak.load(std::memory_order_relaxed);
    while (static_cast<std::size_t>(live) > peak &&
           !t->peak.compare_exchange_weak(peak, static_cast<std::size_t>(live),
                                          std::memory_order_relaxed)) {
    }
    auto total = g_live_total.fetch_add(static_cast<std::ptrdiff_t>(size),
                                        std::memory_order_relaxed) +
                 static_cast<std::ptrdiff_t>(size);
    if (static_cast<std::size_t>(total) >=
        g_snapshot_floor.load(std::memory_order_relaxed)) [[unlikely]] {
        snapshot(static_cast<std::size_t>(total));
    }
}

void count_free(std::uintptr_t site, std::size_t size) noexcept {
    g_live_total.fetch_sub(static_cast<std::ptrdiff_t>(size),
                           std::memory_order_relaxed);
    if (size > DEBUG_BIG_BLOCK_BYTES) {
        g_live_big.fetch_sub(static_cast<std::ptrdiff_t>(size),
                             std::memory_order_relaxed);
    }
    SiteTotal* t = total_of(site);
    if (t != nullptr) {
        t->live.fetch_sub(static_cast<std::ptrdiff_t>(size),
                          std::memory_order_relaxed);
    }
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

std::atomic<std::uintptr_t>* held_list() noexcept {
    auto* h = g_held.load(std::memory_order_acquire);
    if (h != nullptr) [[likely]]
        return h;
    constexpr std::size_t bytes =
        DEBUG_HELD_BLOCKS * sizeof(std::atomic<std::uintptr_t>);
    void* raw = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) {
        abort_with("dftracer-utils memory: cannot map the held list\n");
    }
    std::atomic<std::uintptr_t>* expected = nullptr;
    auto* mine = static_cast<std::atomic<std::uintptr_t>*>(raw);
    if (!g_held.compare_exchange_strong(expected, mine,
                                        std::memory_order_acq_rel)) {
        ::munmap(raw, bytes);
        return expected;
    }
    return mine;
}

// Marks the block freed, checks it and fills it. The block is not released.
void retire(void* p, std::size_t size, bool sized) noexcept {
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
    count_free(site, have);
    std::memset(p, DEBUG_FREED_FILL, have + DEBUG_REDZONE_BYTES);
}

}  // namespace

std::uint64_t debug_allocation_count() noexcept {
    return g_allocations.load(std::memory_order_relaxed);
}

void* debug_allocate(std::size_t size, std::size_t align, void* site) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
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
    count_allocation(reinterpret_cast<std::uintptr_t>(site), size);
    std::memset(static_cast<char*>(p) + size, DEBUG_REDZONE_FILL,
                DEBUG_REDZONE_BYTES);
    return p;
}

void debug_deallocate(void* p, std::size_t size, bool sized) noexcept {
    if (p == nullptr) return;
    retire(p, size, sized);
    std::uint32_t slot = g_cursor.fetch_add(1, std::memory_order_relaxed) %
                         DEBUG_QUARANTINE_BLOCKS;
    auto old = g_quarantine[slot].exchange(reinterpret_cast<std::uintptr_t>(p),
                                           std::memory_order_acq_rel);
    if (old != 0) release(reinterpret_cast<void*>(old));
}

void debug_retain(void* p, std::size_t size) noexcept {
    if (p == nullptr) return;
    retire(p, size, true);
    std::size_t at = g_held_count.fetch_add(1, std::memory_order_relaxed);
    if (at >= DEBUG_HELD_BLOCKS) {
        abort_with("dftracer-utils memory: the held list is full\n");
    }
    held_list()[at].store(reinterpret_cast<std::uintptr_t>(p),
                          std::memory_order_release);
}

void debug_drain_held(std::uint16_t pool) noexcept {
    auto* list = g_held.load(std::memory_order_acquire);
    if (list == nullptr) return;
    std::size_t count = std::min(g_held_count.load(std::memory_order_relaxed),
                                 DEBUG_HELD_BLOCKS);
    for (std::size_t i = 0; i < count; ++i) {
        auto entry = list[i].load(std::memory_order_acquire);
        if (entry == 0) continue;
        if (pool != DEBUG_ALL_POOLS) {
            Slot* s = lookup(reinterpret_cast<void*>(entry), false);
            if (s->pool.load(std::memory_order_relaxed) != pool) continue;
        }
        if (list[i].compare_exchange_strong(entry, 0,
                                            std::memory_order_acq_rel)) {
            release(reinterpret_cast<void*>(entry));
        }
    }
}

// Always a new block: the redzone sits right behind the old size.
void* debug_reallocate(void* p, std::size_t old_size, std::size_t new_size,
                       std::size_t align, void* site) {
    if (p == nullptr) return debug_allocate(new_size, align, site);
    Slot* s = lookup(p, false);
    if (s == nullptr ||
        s->state.load(std::memory_order_acquire) != STATE_LIVE) {
        die("resize of a block that is not live", p, old_size, 0);
    }
    if (s->size.load(std::memory_order_relaxed) != old_size) {
        die("sized free does not match the allocated size", p,
            s->size.load(std::memory_order_relaxed),
            s->site.load(std::memory_order_relaxed));
    }
    void* moved;
    {
        ScopeOverride scope{attrib_local(),
                            s->pool.load(std::memory_order_relaxed)};
        moved = debug_allocate(new_size, align, site);
    }
    std::memcpy(moved, p, std::min(old_size, new_size));
    debug_deallocate(p, old_size, true);
    return moved;
}

void debug_drain() noexcept {
    if (g_table.load(std::memory_order_acquire) == nullptr) return;
    for (auto& entry : g_quarantine) {
        auto old = entry.exchange(0, std::memory_order_acq_rel);
        if (old != 0) release(reinterpret_cast<void*>(old));
    }
}

void debug_top_sites(std::size_t count, bool by_peak, DebugTotalVisit visit,
                     void* context) noexcept {
    SiteTotal* t = g_totals.load(std::memory_order_acquire);
    if (t == nullptr) return;
    constexpr std::size_t MAX_TOP = 64;
    count = std::min(count, MAX_TOP);
    DebugSiteTotal best[MAX_TOP];
    std::size_t have = 0;
    auto key = [by_peak](const DebugSiteTotal& v) {
        return by_peak ? v.peak : v.live;
    };
    for (std::size_t i = 0; i < DEBUG_SITE_TOTALS; ++i) {
        std::uintptr_t site = t[i].site.load(std::memory_order_acquire);
        if (site == 0) continue;
        auto live = t[i].live.load(std::memory_order_relaxed);
        DebugSiteTotal v{site, live > 0 ? static_cast<std::size_t>(live) : 0,
                         t[i].peak.load(std::memory_order_relaxed),
                         t[i].allocations.load(std::memory_order_relaxed)};
        if (key(v) == 0) continue;
        std::size_t at = have;
        while (at > 0 && key(best[at - 1]) < key(v)) --at;
        if (at >= count) continue;
        if (have < count) ++have;
        for (std::size_t j = have - 1; j > at; --j) best[j] = best[j - 1];
        best[at] = v;
    }
    for (std::size_t i = 0; i < have; ++i) visit(context, best[i]);
}

DebugPeak debug_top_sites_at_peak(std::size_t count, DebugTotalVisit visit,
                                  void* context) noexcept {
    DebugPeak peak{g_peak_total.load(std::memory_order_acquire),
                   g_peak_big.load(std::memory_order_relaxed)};
    SiteTotal* t = g_totals.load(std::memory_order_acquire);
    if (t == nullptr || g_at_peak == nullptr || peak.total == 0) return peak;
    constexpr std::size_t MAX_TOP = 64;
    count = std::min(count, MAX_TOP);
    DebugSiteTotal best[MAX_TOP];
    std::size_t have = 0;
    for (std::size_t i = 0; i < DEBUG_SITE_TOTALS; ++i) {
        std::uintptr_t site = t[i].site.load(std::memory_order_acquire);
        if (site == 0) continue;
        auto live = g_at_peak[i].load(std::memory_order_relaxed);
        if (live <= 0) continue;
        DebugSiteTotal v{site, static_cast<std::size_t>(live),
                         t[i].peak.load(std::memory_order_relaxed),
                         t[i].allocations.load(std::memory_order_relaxed)};
        std::size_t at = have;
        while (at > 0 && best[at - 1].live < v.live) --at;
        if (at >= count) continue;
        if (have < count) ++have;
        for (std::size_t j = have - 1; j > at; --j) best[j] = best[j - 1];
        best[at] = v;
    }
    for (std::size_t i = 0; i < have; ++i) visit(context, best[i]);
    return peak;
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
