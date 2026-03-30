#ifndef DFTRACER_UTILS_CORE_COMMON_SHARDED_MUTEX_H
#define DFTRACER_UTILS_CORE_COMMON_SHARDED_MUTEX_H

#include <dftracer/utils/core/common/platform_compat.h>

#include <array>
#include <cstddef>
#include <functional>
#include <mutex>

namespace dftracer::utils {

/**
 * ShardedMutex<T>
 *
 * Generic sharded mutex pattern for reducing lock contention
 *
 * @tparam T Type of data stored per shard
 * @tparam NUM_SHARDS Number of shards (must be power of 2)

 * Usage:
 * @code
 * ShardedMutex<std::unordered_map<int, Callback>> callbacks;
 *
 * // Write to shard
 * callbacks.with_shard(task_id, [&](auto& map) {
 *     map[task_id].push_back(callback);
 * });
 *
 * // Read from shard
 * callbacks.with_shard(task_id, [&](const auto& map) {
 *     if (auto it = map.find(task_id); it != map.end()) {
 *         process(it->second);
 *     }
 * });
 * @endcode
 */
template <typename T, std::size_t NUM_SHARDS = 64>
class ShardedMutex {
    static_assert((NUM_SHARDS & (NUM_SHARDS - 1)) == 0,
                  "NUM_SHARDS must be power of 2 for fast modulo");

   protected:
    struct Shard {
        T data;
        mutable std::mutex mutex;

        // Padding to prevent false sharing between adjacent shards
        alignas(
            DFTRACER_CACHE_LINE_SIZE) char padding_[DFTRACER_CACHE_LINE_SIZE];
    };

    std::array<Shard, NUM_SHARDS> shards_;

    /**
     * Fast shard selection using bit masking
     * O(1) with just AND operation (vs expensive modulo)
     *
     * Example: NUM_SHARDS = 64 = 0b1000000
     *          MASK = 63 = 0b0111111
     *          key & MASK gives range [0, 63]
     */
    Shard& get_shard(std::size_t key) noexcept {
        return shards_[key & (NUM_SHARDS - 1)];
    }

    const Shard& get_shard(std::size_t key) const noexcept {
        return shards_[key & (NUM_SHARDS - 1)];
    }

   public:
    ShardedMutex() = default;

    // Move only transfers shard data, not mutexes. std::mutex is
    // non-movable (OS handle tied to address). The destination gets
    // fresh mutexes. Only safe when no thread holds any lock on either
    // the source or destination (e.g., after all parallel work is done).
    ShardedMutex(ShardedMutex&& other) noexcept {
        for (std::size_t i = 0; i < NUM_SHARDS; ++i) {
            shards_[i].data = std::move(other.shards_[i].data);
        }
    }

    ShardedMutex& operator=(ShardedMutex&& other) noexcept {
        if (this != &other) {
            for (std::size_t i = 0; i < NUM_SHARDS; ++i) {
                shards_[i].data = std::move(other.shards_[i].data);
            }
        }
        return *this;
    }

    ShardedMutex(const ShardedMutex&) = delete;
    ShardedMutex& operator=(const ShardedMutex&) = delete;

    /**
     * Execute function with exclusive access to shard
     *
     * @param key Shard key (typically task ID, hash, etc.)
     * @param func Function to execute: [](T& data) { ... }
     *
     * Thread-safe: Only locks the specific shard, other threads can access
     * different shards concurrently.
     */
    template <typename Func>
    void with_shard(std::size_t key, Func&& func) {
        auto& shard = get_shard(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        func(shard.data);
    }

    /**
     * Execute function with shared (const) access to shard
     *
     * @param key Shard key
     * @param func Function to execute: [](const T& data) { ... }
     */
    template <typename Func>
    void with_shard(std::size_t key, Func&& func) const {
        const auto& shard = get_shard(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        func(shard.data);
    }

    /**
     * Execute function on ALL shards sequentially
     * Useful for cleanup, aggregation, or global operations
     *
     * @param func Function to execute: [](T& data) { ... }
     *
     * Acquires locks sequentially to avoid deadlock.
     * Not thread-safe with respect to other operations!
     */
    template <typename Func>
    void for_each_shard(Func&& func) {
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            func(shard.data);
        }
    }

    /**
     * Execute function on ALL shards sequentially (const version)
     */
    template <typename Func>
    void for_each_shard(Func&& func) const {
        for (const auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            func(shard.data);
        }
    }

    /**
     * Execute function on ALL shards without locking.
     * Only safe when no concurrent access is possible.
     */
    template <typename Func>
    void for_each_shard_unlocked(Func&& func) {
        for (auto& shard : shards_) {
            func(shard.data);
        }
    }

    template <typename Func>
    void for_each_shard_unlocked(Func&& func) const {
        for (const auto& shard : shards_) {
            func(shard.data);
        }
    }

    /**
     * Get number of shards (compile-time constant)
     */
    static constexpr std::size_t num_shards() noexcept { return NUM_SHARDS; }

    /**
     * Try to execute function with exclusive access to shard (non-blocking)
     *
     * @param key Shard key
     * @param func Function to execute if lock acquired
     * @return true if function was executed, false if lock not acquired
     */
    template <typename Func>
    bool try_with_shard(std::size_t key, Func&& func) {
        auto& shard = get_shard(key);
        std::unique_lock<std::mutex> lock(shard.mutex, std::try_to_lock);
        if (lock.owns_lock()) {
            func(shard.data);
            return true;
        }
        return false;
    }

    /**
     * Clear all shards (calls .clear() on each shard's data)
     * Only available if T has a .clear() method
     */
    template <typename U = T>
    auto clear() -> decltype(std::declval<U>().clear(), void()) {
        for_each_shard([](T& data) { data.clear(); });
    }

    /**
     * Get approximate total size across all shards
     * Only available if T has a .size() method
     *
     * Note: Not thread-safe
     * Result is approximate due to concurrent modifications.
     */
    template <typename U = T>
    auto size() const -> decltype(std::declval<U>().size()) {
        using SizeType = decltype(std::declval<U>().size());
        SizeType total = 0;
        for_each_shard([&total](const T& data) { total += data.size(); });
        return total;
    }

    /**
     * Check if all shards are empty
     * Only available if T has an .empty() method
     */
    template <typename U = T>
    auto empty() const -> decltype(std::declval<U>().empty()) {
        bool all_empty = true;
        for_each_shard([&all_empty](const T& data) {
            if (!data.empty()) {
                all_empty = false;
            }
        });
        return all_empty;
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_SHARDED_MUTEX_H
