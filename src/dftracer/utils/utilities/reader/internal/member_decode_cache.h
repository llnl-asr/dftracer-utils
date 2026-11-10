// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_MEMBER_DECODE_CACHE_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_MEMBER_DECODE_CACHE_H

#include <dftracer/utils/coro/async_mutex.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/cache/eviction_policy.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/vector.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

namespace dftracer::utils::utilities::reader::internal {

/// A decoded gzip member plus the compressed byte count it consumed. The
/// consumed size is cached alongside the bytes because a cache *hit* still
/// needs it to advance the reader to the next member.
struct DecodedMember {
    dstd::Vector<std::uint8_t> data;
    std::uint64_t compressed_size = 0;
};

/// Cache key: a file token (e.g. hash of path) plus a member index.
struct MemberKey {
    std::uint64_t file;
    std::uint64_t member;
    bool operator==(const MemberKey& o) const {
        return file == o.file && member == o.member;
    }
};

/// Hasher of the shard tables. It also picks the shard.
struct MemberKeyHash {
    using is_avalanching = void;

    std::size_t operator()(const MemberKey& k) const noexcept {
        return static_cast<std::size_t>(
            dstd::hash::splitmix64(k.file ^ dstd::hash::splitmix64(k.member)));
    }
};

}  // namespace dftracer::utils::utilities::reader::internal

namespace dftracer::utils::dstd {
template <>
struct Hash<utilities::reader::internal::MemberKey>
    : utilities::reader::internal::MemberKeyHash {};
}  // namespace dftracer::utils::dstd

namespace dftracer::utils::utilities::reader::internal {

/// Coalescing, sharded, bounded cache for decoded gzip members.
///
/// Concurrent requests for the same member run the decode exactly once
/// (single-flight): callers share one Entry and serialize on its
/// @ref coro::AsyncMutex - the first decodes while holding it (thread freed via
/// async suspend, not blocked), the rest wake and read the shared immutable
/// buffer. So N overlapping queries over one member cost one pread + one
/// inflate instead of N.
///
/// The key space is split into NSHARDS independent shards (each a dstd::HashMap
/// + eviction policy under its own std::mutex), so lookups on different
/// members do not contend. The byte budget is one total: a shard holding a few
/// large members keeps them while the cache as a whole has room. The eviction
/// policy (LRU or CLOCK) is chosen at construction; the map/policy lock is held
/// only for the brief bookkeeping, never across a decode.
class MemberDecodeCache {
   public:
    using Bytes = dstd::SharedPtr<const DecodedMember>;
    using Producer = dstd::Function<coro::CoroTask<dstd::Result<Bytes>>()>;

    static constexpr std::size_t NSHARDS = 16;
    static_assert((NSHARDS & (NSHARDS - 1)) == 0,
                  "NSHARDS must be a power of 2");

    /// `capacity_bytes` bounds total retained decoded bytes. 0 disables
    /// retention but still coalesces in-flight decodes.
    explicit MemberDecodeCache(
        std::size_t capacity_bytes,
        dstd::cache::EvictionKind kind = dstd::cache::EvictionKind::Lru)
        : capacity_(capacity_bytes) {
        for (auto& shard : shards_) {
            shard.policy = dstd::cache::make_eviction_policy<MemberKey>(kind);
        }
    }

    struct Stats {
        std::uint64_t requests = 0;
        std::uint64_t decodes = 0;
        std::uint64_t hits = 0;
        std::uint64_t evictions = 0;
    };

    /// `produce` is taken by value: this is a coroutine, so a by-reference
    /// parameter bound to a temporary would dangle before the body runs. When
    /// `produce` fails, the error is returned and nothing is cached, so the
    /// next request decodes again.
    coro::CoroTask<dstd::Result<Bytes>> get_or_decode(std::uint64_t file_token,
                                                      std::uint64_t member_idx,
                                                      Producer produce) {
        const MemberKey key{file_token, member_idx};
        const std::size_t h = dstd::Hash<MemberKey>{}(key);
        Shard& shard = shards_[(h >> 32) & (NSHARDS - 1)];

        requests_.fetch_add(1, std::memory_order_relaxed);
        dstd::SharedPtr<Entry> entry;
        {
            std::lock_guard<std::mutex> g(shard.mu);
            auto it = shard.map.find(key);
            if (it != shard.map.end()) {
                entry = it->second;
                if (entry->ready.load(std::memory_order_acquire)) {
                    hits_.fetch_add(1, std::memory_order_relaxed);
                    shard.policy->touch(key);
                }
            } else {
                entry = dstd::make_shared<Entry>();
                shard.map.emplace(key, entry);
                // Not tracked for eviction until ready, so in-flight entries
                // are never evicted out from under their waiters.
            }
        }

        co_await entry->mutex.lock();
        coro::AsyncMutexGuard guard(entry->mutex);
        if (!entry->ready.load(std::memory_order_relaxed)) {
            auto produced = co_await produce();
            if (!produced) co_return produced.error();
            entry->bytes = std::move(*produced);
            entry->size = entry->bytes ? entry->bytes->data.size() : 0;
            entry->ready.store(true, std::memory_order_release);
            decodes_.fetch_add(1, std::memory_order_relaxed);
            if (capacity_ == 0) {
                std::lock_guard<std::mutex> g(shard.mu);
                shard.map.erase(key);  // retain nothing; coalesce-only
            } else {
                total_bytes_.fetch_add(entry->size, std::memory_order_relaxed);
                // This key is not in a policy yet, so it is never the victim
                // of its own insert. One shard lock at a time: no lock order.
                const std::size_t own = (h >> 32) & (NSHARDS - 1);
                for (std::size_t i = 0; i < NSHARDS && over_budget(); ++i) {
                    Shard& s = shards_[(own + i) & (NSHARDS - 1)];
                    std::lock_guard<std::mutex> g(s.mu);
                    evict_over_budget(s);
                }
                std::lock_guard<std::mutex> g(shard.mu);
                shard.policy->touch(key);
            }
        }
        co_return entry->bytes;
    }

    Stats stats() const {
        return Stats{requests_.load(std::memory_order_relaxed),
                     decodes_.load(std::memory_order_relaxed),
                     hits_.load(std::memory_order_relaxed),
                     evictions_.load(std::memory_order_relaxed)};
    }

   private:
    struct Entry {
        coro::AsyncMutex mutex;
        // Written under `mutex`, also read under the shard lock.
        std::atomic<bool> ready{false};
        Bytes bytes;
        std::size_t size = 0;
    };
    struct Shard {
        std::mutex mu;
        dstd::HashMap<MemberKey, dstd::SharedPtr<Entry>, MemberKeyHash> map;
        dstd::UniquePtr<dstd::cache::EvictionPolicy<MemberKey>> policy;
    };

    bool over_budget() const {
        return total_bytes_.load(std::memory_order_relaxed) > capacity_;
    }

    /// Evicts `s`'s ready entries, least recent first, while the cache is over
    /// budget. The caller holds `s.mu`.
    void evict_over_budget(Shard& s) {
        while (over_budget()) {
            auto victim = s.policy->pop_victim();
            if (!victim) return;
            auto vit = s.map.find(*victim);
            if (vit == s.map.end()) continue;
            total_bytes_.fetch_sub(vit->second->size,
                                   std::memory_order_relaxed);
            s.map.erase(vit);
            evictions_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::array<Shard, NSHARDS> shards_;
    std::size_t capacity_;
    std::atomic<std::size_t> total_bytes_{0};
    std::atomic<std::uint64_t> requests_{0};
    std::atomic<std::uint64_t> decodes_{0};
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> evictions_{0};
};

/// Process-level decode cache, or nullptr when unconfigured. Readers pass it
/// to the inflater so concurrent server queries share member decodes; a null
/// return keeps the plain per-reader decode path (CLI, tests).
MemberDecodeCache* global_member_decode_cache() noexcept;

/// Enable the process-level cache with `capacity_bytes` of retained decoded
/// members and the given eviction policy. Idempotent: the first call fixes the
/// instance; later calls just ensure it is published. Call once at startup.
/// Shutdown frees the cache, after which the global is nullptr again.
void configure_global_member_decode_cache(
    std::size_t capacity_bytes,
    dstd::cache::EvictionKind kind = dstd::cache::EvictionKind::Lru);

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_MEMBER_DECODE_CACHE_H
