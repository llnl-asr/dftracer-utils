#ifndef DFTRACER_UTILS_MEMORY_ATTRIB_H
#define DFTRACER_UTILS_MEMORY_ATTRIB_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dftracer::utils::memory {

inline constexpr std::uint16_t ROOT_POOL_ID = 0;
inline constexpr std::uint16_t GENERAL_POOL_ID = 1;
inline constexpr std::size_t POOL_SLOTS = 65535;
inline constexpr std::size_t ATTRIB_ENTRIES = 16;
inline constexpr std::ptrdiff_t ATTRIB_FLUSH_BYTES = std::ptrdiff_t{1} << 20;

// An entry packs its byte and block deltas into one word, bytes in the low
// ATTRIB_BYTE_BITS bits (signed) and blocks above them. Every charge or credit
// that reaches an entry is at most ATTRIB_FLUSH_BYTES, and an entry flushes as
// soon as its bytes pass that bound, so the bytes stay within twice the bound,
// far inside the 24-bit field. A bigger block goes straight to its pool slot.
inline constexpr unsigned ATTRIB_BYTE_BITS = 24;
inline constexpr std::int64_t ATTRIB_BLOCK = std::int64_t{1}
                                             << ATTRIB_BYTE_BITS;

/// The word delta of one block of @p bytes, at most ATTRIB_FLUSH_BYTES.
constexpr std::int64_t attrib_delta(std::size_t bytes) noexcept {
    return static_cast<std::int64_t>(bytes) + ATTRIB_BLOCK;
}

constexpr std::int64_t attrib_bytes(std::int64_t word) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(word)
                                     << (64 - ATTRIB_BYTE_BITS)) >>
           (64 - ATTRIB_BYTE_BITS);
}

constexpr std::int64_t attrib_blocks(std::int64_t word) noexcept {
    return (word - attrib_bytes(word)) >> ATTRIB_BYTE_BITS;
}

/// Bytes and blocks one thread has charged to a pool and not yet added to the
/// pool's counters. Only the owning thread writes it, with relaxed stores.
struct alignas(16) AttribEntry {
    std::atomic<std::uint32_t> id{0};
    std::atomic<std::int64_t> word{0};
};

/// Per-thread attribution state, kept in the thread's heap. `hot` belongs to
/// the current scope, so a charge needs no lookup. The other entries are
/// mapped by pool id, so a miss flushes the entry that was there. A cache is
/// linked into one process-wide list when it is created and never leaves it.
struct AttribCache {
    AttribCache() noexcept {
        hot.id.store(GENERAL_POOL_ID, std::memory_order_relaxed);
    }

    AttribEntry hot;
    AttribEntry entries[ATTRIB_ENTRIES];
    std::atomic<AttribCache*> next{nullptr};
    std::uint16_t scope = GENERAL_POOL_ID;
};

/// Add the entry's counts to its pool's counters and zero it.
void attrib_flush(AttribEntry& entry) noexcept;
/// Flush the entry and make it name @p id.
void attrib_switch(AttribEntry& entry, std::uint16_t id) noexcept;
/// Make @p id the cache's scope.
void attrib_set_scope(AttribCache& cache, std::uint16_t id) noexcept;
/// Makes @p id the cache's scope until destruction, as a Scope does, and
/// skips both switches when it already is.
class ScopeOverride {
   public:
    ScopeOverride(AttribCache& cache, std::uint16_t id) noexcept
        : cache_(cache), previous_(cache.scope) {
        if (id != previous_) attrib_set_scope(cache, id);
    }
    ScopeOverride(const ScopeOverride&) = delete;
    ScopeOverride& operator=(const ScopeOverride&) = delete;
    ~ScopeOverride() {
        if (cache_.scope != previous_) attrib_set_scope(cache_, previous_);
    }

   private:
    AttribCache& cache_;
    std::uint16_t previous_;
};

/// Link a new cache into the list that counter reads walk.
void attrib_register(AttribCache& cache) noexcept;
/// Bytes charged to every pool and not yet credited, from the flushed
/// counters and every cache.
std::ptrdiff_t attrib_live_total() noexcept;

/// The calling thread's cache for the active backend.
AttribCache& attrib_local();
AttribCache& page_attrib();
AttribCache& system_attrib();
/// After fork() in the child: caches of threads that did not survive.
void system_attrib_after_fork() noexcept;

/// Add @p bytes and @p blocks to the counters of pool @p id at once, for a
/// block too big for an entry.
void attrib_direct(std::uint16_t id, std::ptrdiff_t bytes,
                   std::ptrdiff_t blocks) noexcept;

/// Charge one block to the cache's scope; @p delta is attrib_delta of a size
/// of at most ATTRIB_FLUSH_BYTES.
inline void attrib_charge(AttribCache& cache, std::int64_t delta) noexcept {
    AttribEntry& e = cache.hot;
    std::int64_t word = e.word.load(std::memory_order_relaxed) + delta;
    e.word.store(word, std::memory_order_relaxed);
    if (attrib_bytes(word) >= ATTRIB_FLUSH_BYTES) [[unlikely]]
        attrib_flush(e);
}

/// Credit one block to pool @p id, which is not always the scope; @p delta is
/// as for attrib_charge.
inline void attrib_credit(AttribCache& cache, std::uint16_t id,
                          std::int64_t delta) noexcept {
    AttribEntry* e = &cache.hot;
    if (id != cache.scope) {
        e = &cache.entries[id % ATTRIB_ENTRIES];
        if (e->id.load(std::memory_order_relaxed) != id) [[unlikely]] {
            attrib_switch(*e, id);
        }
    }
    std::int64_t word = e->word.load(std::memory_order_relaxed) - delta;
    e->word.store(word, std::memory_order_relaxed);
    if (attrib_bytes(word) <= -ATTRIB_FLUSH_BYTES) [[unlikely]] {
        attrib_flush(*e);
    }
}

/// Charge a block of any size to the cache's scope.
inline void attrib_charge_any(AttribCache& cache, std::size_t bytes) noexcept {
    if (bytes <= static_cast<std::size_t>(ATTRIB_FLUSH_BYTES)) [[likely]] {
        attrib_charge(cache, attrib_delta(bytes));
    } else {
        attrib_direct(cache.scope, static_cast<std::ptrdiff_t>(bytes), 1);
    }
}

/// Credit a block of any size to pool @p id.
inline void attrib_credit_any(AttribCache& cache, std::uint16_t id,
                              std::size_t bytes) noexcept {
    if (bytes <= static_cast<std::size_t>(ATTRIB_FLUSH_BYTES)) [[likely]] {
        attrib_credit(cache, id, attrib_delta(bytes));
    } else {
        attrib_direct(id, -static_cast<std::ptrdiff_t>(bytes), -1);
    }
}

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_ATTRIB_H
