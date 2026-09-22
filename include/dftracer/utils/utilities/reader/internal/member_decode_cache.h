#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_MEMBER_DECODE_CACHE_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_MEMBER_DECODE_CACHE_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/cache/eviction_policy.h>
#include <dftracer/utils/core/common/hash/constants.h>
#include <dftracer/utils/core/coro/async_mutex.h>
#include <dftracer/utils/core/coro/task.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace dftracer::utils::utilities::reader::internal {

/// A decoded gzip member plus the compressed byte count it consumed. The
/// consumed size is cached alongside the bytes because a cache *hit* still
/// needs it to advance the reader to the next member.
struct DecodedMember {
    std::vector<std::uint8_t> data;
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

}  // namespace dftracer::utils::utilities::reader::internal

namespace std {
template <>
struct hash<dftracer::utils::utilities::reader::internal::MemberKey> {
    std::size_t operator()(
        const dftracer::utils::utilities::reader::internal::MemberKey& k)
        const noexcept {
        std::uint64_t h = k.file * dftracer::utils::hash::GOLDEN_RATIO;
        h ^= k.member + dftracer::utils::hash::GOLDEN_RATIO + (h << 6) +
             (h >> 2);
        return static_cast<std::size_t>(h);
    }
};
}  // namespace std

namespace dftracer::utils::utilities::reader::internal {

/// Coalescing, sharded, bounded cache for decoded gzip members.
///
/// Concurrent requests for the same member run the decode exactly once
/// (single-flight): callers share one Entry and serialize on its
/// coro::AsyncMutex - the first decodes while holding it (thread freed via
/// async suspend, not blocked), the rest wake and read the shared immutable
/// buffer. So N overlapping queries over one member cost one pread + one
/// inflate instead of N.
///
/// The key space is split into NSHARDS independent shards (each an ankerl map
/// + eviction policy + byte budget under its own std::mutex), so lookups on
/// different members do not contend. The eviction policy (LRU or CLOCK) is
/// chosen at construction; the map/policy lock is held only for the brief
/// bookkeeping, never across a decode.
class MemberDecodeCache {
   public:
    using Bytes = std::shared_ptr<const DecodedMember>;
    using Producer = std::function<coro::CoroTask<Bytes>()>;

    static constexpr std::size_t NSHARDS = 16;
    static_assert((NSHARDS & (NSHARDS - 1)) == 0,
                  "NSHARDS must be a power of 2");

    /// `capacity_bytes` bounds total retained decoded bytes (split evenly
    /// across shards). 0 disables retention but still coalesces in-flight
    /// decodes.
    explicit MemberDecodeCache(
        std::size_t capacity_bytes,
        cache::EvictionKind kind = cache::EvictionKind::LRU)
        : per_shard_capacity_(capacity_bytes / NSHARDS) {
        for (auto& shard : shards_) {
            shard.policy = cache::make_eviction_policy<MemberKey>(kind);
        }
    }

    struct Stats {
        std::uint64_t requests = 0;
        std::uint64_t decodes = 0;
        std::uint64_t hits = 0;
        std::uint64_t evictions = 0;
    };

    /// `produce` is taken by value: this is a coroutine, so a by-reference
    /// parameter bound to a temporary would dangle before the body runs.
    coro::CoroTask<Bytes> get_or_decode(std::uint64_t file_token,
                                        std::uint64_t member_idx,
                                        Producer produce) {
        const MemberKey key{file_token, member_idx};
        const std::size_t h = std::hash<MemberKey>{}(key);
        Shard& shard = shards_[h & (NSHARDS - 1)];

        requests_.fetch_add(1, std::memory_order_relaxed);
        std::shared_ptr<Entry> entry;
        {
            std::lock_guard<std::mutex> g(shard.mu);
            auto it = shard.map.find(key);
            if (it != shard.map.end()) {
                entry = it->second;
                if (entry->ready) {
                    hits_.fetch_add(1, std::memory_order_relaxed);
                    shard.policy->touch(key);
                }
            } else {
                entry = std::make_shared<Entry>();
                shard.map.emplace(key, entry);
                // Not tracked for eviction until ready, so in-flight entries
                // are never evicted out from under their waiters.
            }
        }

        co_await entry->mutex.lock();
        coro::AsyncMutexGuard guard(entry->mutex);
        if (!entry->ready) {
            entry->bytes = co_await produce();
            entry->size = entry->bytes ? entry->bytes->data.size() : 0;
            entry->ready = true;
            decodes_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> g(shard.mu);
            if (per_shard_capacity_ == 0) {
                shard.map.erase(key);  // retain nothing; coalesce-only
            } else {
                shard.cur_bytes += entry->size;
                // Evict other ready entries (this key is not in the policy
                // yet, so it is never the victim of its own insert).
                while (shard.cur_bytes > per_shard_capacity_) {
                    auto victim = shard.policy->pop_victim();
                    if (!victim) break;
                    auto vit = shard.map.find(*victim);
                    if (vit == shard.map.end()) continue;
                    shard.cur_bytes -= vit->second->size;
                    shard.map.erase(vit);
                    evictions_.fetch_add(1, std::memory_order_relaxed);
                }
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
        bool ready = false;
        Bytes bytes;
        std::size_t size = 0;
    };
    struct Shard {
        std::mutex mu;
        ankerl::unordered_dense::map<MemberKey, std::shared_ptr<Entry>> map;
        std::unique_ptr<cache::EvictionPolicy<MemberKey>> policy;
        std::size_t cur_bytes = 0;
    };

    std::array<Shard, NSHARDS> shards_;
    std::size_t per_shard_capacity_;
    std::atomic<std::uint64_t> requests_{0};
    std::atomic<std::uint64_t> decodes_{0};
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> evictions_{0};
};

namespace detail {
inline std::atomic<MemberDecodeCache*>& global_cache_slot() noexcept {
    static std::atomic<MemberDecodeCache*> slot{nullptr};
    return slot;
}
}  // namespace detail

/// Process-level decode cache, or nullptr when unconfigured. Readers pass it
/// to the inflater so concurrent server queries share member decodes; a null
/// return keeps the plain per-reader decode path (CLI, tests).
inline MemberDecodeCache* global_member_decode_cache() noexcept {
    return detail::global_cache_slot().load(std::memory_order_acquire);
}

/// Enable the process-level cache with `capacity_bytes` of retained decoded
/// members and the given eviction policy. Idempotent: the first call fixes the
/// instance; later calls just ensure it is published. Call once at startup.
inline void configure_global_member_decode_cache(
    std::size_t capacity_bytes,
    cache::EvictionKind kind = cache::EvictionKind::LRU) {
    static MemberDecodeCache instance(capacity_bytes, kind);
    detail::global_cache_slot().store(&instance, std::memory_order_release);
}

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_MEMBER_DECODE_CACHE_H
