#ifndef DFTRACER_UTILS_CORE_COMMON_SHARDED_MAP_H
#define DFTRACER_UTILS_CORE_COMMON_SHARDED_MAP_H

#include <dftracer/utils/core/common/sharded_mutex.h>

namespace dftracer::utils {

/**
 * @brief Thread-safe sharded map extending ShardedMutex with map ergonomics.
 *
 * Adds key-based shard selection (via Hash), operator[], and emplace.
 * The underlying ShardedMutex handles locking and shard storage.
 *
 * @tparam Map The underlying map type per shard
 * @tparam Hash Hash function for map keys
 * @tparam NUM_SHARDS Number of shards (power of 2)
 */
template <typename Map, typename Hash, std::size_t NUM_SHARDS = 64>
class ShardedMap : public ShardedMutex<Map, NUM_SHARDS> {
    using Base = ShardedMutex<Map, NUM_SHARDS>;

   public:
    using key_type = typename Map::key_type;
    using mapped_type = typename Map::mapped_type;

    ShardedMap() = default;

    template <typename Fn>
    void with_shard(const key_type& key, Fn&& fn) {
        Base::with_shard(hasher_(key), std::forward<Fn>(fn));
    }

    template <typename Fn>
    void with_shard(const key_type& key, Fn&& fn) const {
        Base::with_shard(hasher_(key), std::forward<Fn>(fn));
    }

    mapped_type& operator[](const key_type& key) {
        auto& shard = Base::get_shard(hasher_(key));
        std::lock_guard<std::mutex> lock(shard.mutex);
        return shard.data[key];
    }

    template <typename... Args>
    auto emplace(const key_type& key, Args&&... args) {
        auto& shard = Base::get_shard(hasher_(key));
        std::lock_guard<std::mutex> lock(shard.mutex);
        return shard.data.emplace(key, std::forward<Args>(args)...);
    }

   private:
    Hash hasher_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_SHARDED_MAP_H
