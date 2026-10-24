#ifndef DFTRACER_UTILS_DATAFRAME_LAZY_HASH_ARENA_H
#define DFTRACER_UTILS_DATAFRAME_LAZY_HASH_ARENA_H

#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/memory/allocator.h>
#include <dftracer/utils/memory/arena.h>
#include <dftracer/utils/memory/manager.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace dftracer::utils::dataframe::lazy_internal {

/// Exact first-seen state over keys the caller hashes, with no per-key heap
/// allocation. Each distinct key is one record bump-allocated from a chunked
/// memory::Arena and never moved, as ClickHouse, DuckDB and Velox keep
/// hash-table keys apart from the slots; a hash maps to the newest record of
/// the chain of keys that share it, so two keys with the same hash still
/// compare their real bytes. Growth copies no key bytes: only the table
/// rehashes. One record per distinct key, never one per input row: a duplicate
/// pays only a lookup and a compare. Not thread-safe; `hash` must be a pure
/// function of the key bytes.
///
/// The records live in the arena's own child pool below the pool given at
/// construction, and the table in that pool; it must stay open until the
/// arena is destroyed.
class HashArena {
   public:
    /// Below this many buckets the table doubles as ankerl::unordered_dense
    /// grows it; from it on, it grows four times at a time.
    static constexpr std::size_t GROWTH_FROM = std::size_t{1} << 18;

    explicit HashArena(memory::Pool& pool)
        : keys_(pool, "hash arena keys"), head_(Head::allocator_type(pool)) {}
    HashArena() : HashArena(memory::current_pool()) {}

    /// True when `key` was not present (and is now); false on a duplicate.
    bool insert(std::uint64_t hash, const char* key, std::uint32_t len) {
        const auto it = head_.find(hash);
        const char* next = nullptr;
        if (it != head_.end()) {
            next = it->second;
            if (find_in_chain(next, key, len)) return false;
        }
        auto* record =
            static_cast<char*>(keys_.allocate(RECORD_HEADER + len, 1));
        std::memcpy(record, &next, NEXT_BYTES);
        std::memcpy(record + NEXT_BYTES, &len, sizeof(len));
        if (len != 0) std::memcpy(record + RECORD_HEADER, key, len);
        if (it != head_.end()) {
            it->second = record;
            return true;
        }
        if (head_.bucket_count() >= GROWTH_FROM &&
            head_.size() >= table_capacity(head_.bucket_count()))
            head_.reserve(table_capacity(GROWTH * head_.bucket_count()));
        head_.emplace(hash, record);
        return true;
    }

    bool contains(std::uint64_t hash, const char* key,
                  std::uint32_t len) const {
        const auto it = head_.find(hash);
        return it != head_.end() && find_in_chain(it->second, key, len);
    }

    std::size_t table_buckets() const noexcept { return head_.bucket_count(); }

    /// The bytes the arena holds from its pools.
    std::uint64_t bytes() const noexcept {
        return keys_.chunk_bytes() +
               head_.values().capacity() * sizeof(Head::value_type) +
               head_.bucket_count() * sizeof(Head::bucket_type);
    }

    /// At most what bytes() reads after `keys` more inserts of `key_bytes`
    /// in total, none longer than `max_len`, counting the chunks and every
    /// container that would grow, so a caller can reserve before the arena
    /// grows.
    std::uint64_t bytes_after(std::size_t keys, std::size_t key_bytes,
                              std::size_t max_len) const noexcept {
        std::size_t buckets = std::max<std::size_t>(head_.bucket_count(), 4);
        std::size_t cap = table_capacity(buckets);
        const std::size_t entries = head_.size() + keys;
        // The values vector grows by itself. A small table doubles once the
        // entry past its capacity is in; from GROWTH_FROM on, insert()
        // reserves the next table's capacity before that entry.
        std::size_t values = head_.values().capacity();
        while (entries > cap) {
            if (buckets < GROWTH_FROM) {
                values = doubled(values, cap + 1);
                buckets *= 2;
            } else {
                values = doubled(values, cap);
                buckets *= GROWTH;
                values = std::max(values, table_capacity(buckets));
            }
            cap = table_capacity(buckets);
        }
        values = doubled(values, entries);
        return records_after(keys, key_bytes, max_len) +
               values * sizeof(Head::value_type) +
               buckets * sizeof(Head::bucket_type);
    }

    /// bytes_after() with no key known to be shorter than all of them.
    std::uint64_t bytes_after(std::size_t keys,
                              std::size_t key_bytes) const noexcept {
        return bytes_after(keys, key_bytes, key_bytes);
    }

   private:
    // A record is the newest-first chain pointer, the length and the key
    // bytes, packed with no padding and read with memcpy: most keys are a few
    // bytes, so padding to the pointer would cost a third of the arena.
    static constexpr std::size_t NEXT_BYTES = sizeof(const char*);
    static constexpr std::size_t RECORD_HEADER =
        NEXT_BYTES + sizeof(std::uint32_t);
    using Head = dstd::PoolHashMap<std::uint64_t, const char*>;
    // The table grows four times at a time, as Arrow's BinaryMemoTable and
    // ClickHouse's HashTable do, so the old tables a rehash frees add up to a
    // third of the new one instead of all of it. Below GROWTH_FROM the
    // reserve ahead costs more than the old tables: 57 thousand keys held
    // 7.1 MiB grown four times and 3.9 MiB doubled.
    static constexpr std::size_t GROWTH = 4;

    // The entries a table of `buckets` holds before it rehashes, computed as
    // ankerl::unordered_dense does.
    std::size_t table_capacity(std::size_t buckets) const noexcept {
        return static_cast<std::size_t>(static_cast<float>(buckets) *
                                        head_.max_load_factor());
    }

    // A vector grown one element at a time doubles its capacity, in libc++
    // and libstdc++ alike.
    static std::size_t doubled(std::size_t capacity,
                               std::size_t need) noexcept {
        if (need <= capacity) return capacity;
        std::size_t c = capacity == 0 ? 1 : capacity;
        while (c < need) c *= 2;
        return c;
    }

    // Each record takes its header and its bytes. A chunk switch leaves less
    // than one record unused, so a chunk holds at least its size less the
    // header and the largest record. A record too big for the next chunk takes
    // a chunk of its own and still advances the doubling, so the chunks after
    // it count at the largest size.
    std::uint64_t records_after(std::size_t keys, std::size_t key_bytes,
                                std::size_t max_len) const noexcept {
        constexpr std::uint64_t PER = RECORD_HEADER;
        constexpr std::uint64_t CHUNK_HEADER = memory::Arena::CHUNK_HEADER;
        constexpr std::uint64_t MAX_CHUNK = memory::ARENA_MAX_CHUNK;
        const std::uint64_t held = keys_.chunk_bytes();
        std::uint64_t left = static_cast<std::uint64_t>(keys) * PER + key_bytes;
        if (left <= keys_.room()) return held;
        const std::uint64_t largest = PER + max_len;
        std::uint64_t total = held;
        std::uint64_t chunk = keys_.next_chunk();
        if (largest + CHUNK_HEADER > chunk) {
            total += left + static_cast<std::uint64_t>(keys) * CHUNK_HEADER;
            chunk = MAX_CHUNK;
        }
        if (largest + CHUNK_HEADER >= MAX_CHUNK)
            return total + static_cast<std::uint64_t>(keys) * MAX_CHUNK;
        while (left > 0) {
            total += chunk;
            if (chunk > CHUNK_HEADER + largest)
                left -= std::min(left, chunk - CHUNK_HEADER - largest);
            chunk = std::min(chunk * 2, MAX_CHUNK);
        }
        return total;
    }

    static bool find_in_chain(const char* record, const char* key,
                              std::uint32_t len) noexcept {
        while (record != nullptr) {
            std::uint32_t have = 0;
            std::memcpy(&have, record + NEXT_BYTES, sizeof(have));
            if (have == len && (len == 0 || std::memcmp(record + RECORD_HEADER,
                                                        key, len) == 0))
                return true;
            const char* next = nullptr;
            std::memcpy(&next, record, NEXT_BYTES);
            record = next;
        }
        return false;
    }

    // Declared first: the table points into it.
    memory::Arena keys_;
    Head head_;
};

}  // namespace dftracer::utils::dataframe::lazy_internal

#endif  // DFTRACER_UTILS_DATAFRAME_LAZY_HASH_ARENA_H
