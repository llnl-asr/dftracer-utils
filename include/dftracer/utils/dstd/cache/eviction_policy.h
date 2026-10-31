#ifndef DFTRACER_UTILS_DSTD_CACHE_EVICTION_POLICY_H
#define DFTRACER_UTILS_DSTD_CACHE_EVICTION_POLICY_H

#include <cstddef>
#include <memory>
#include <optional>

namespace dftracer::utils::dstd::cache {

/**
 * Eviction policy for a bounded cache: tracks which keys to drop first. The
 * policy orders keys only and the cache owns the values and their weights.
 *
 * Calls happen under the cache's (per-shard) lock, so implementations need not
 * be thread safe.
 */
template <typename Key>
class EvictionPolicy {
   public:
    virtual ~EvictionPolicy() = default;

    /// Record an insert or an access; the key becomes least likely to evict.
    virtual void touch(const Key& key) = 0;
    /// Drop a key from tracking (e.g. removed by the cache directly).
    virtual void remove(const Key& key) = 0;
    /// Remove and return the next key to evict, or nullopt if empty.
    virtual std::optional<Key> pop_victim() = 0;
    virtual std::size_t size() const = 0;
};

enum class EvictionKind { Lru, Clock };

/// Create a policy of the given kind.
template <typename Key>
std::unique_ptr<EvictionPolicy<Key>> make_eviction_policy(EvictionKind kind);

}  // namespace dftracer::utils::dstd::cache

#endif  // DFTRACER_UTILS_DSTD_CACHE_EVICTION_POLICY_H
