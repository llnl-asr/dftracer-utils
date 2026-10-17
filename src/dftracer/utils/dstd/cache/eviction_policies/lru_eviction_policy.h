#ifndef DFTRACER_UTILS_DSTD_CACHE_EVICTION_POLICIES_LRU_EVICTION_POLICY_H
#define DFTRACER_UTILS_DSTD_CACHE_EVICTION_POLICIES_LRU_EVICTION_POLICY_H

#include <dftracer/utils/dstd/cache/eviction_policy.h>

#include <cstddef>
#include <list>
#include <optional>
#include <unordered_map>

namespace dftracer::utils::cache {

/// Least-recently-used: touch moves a key to the front; the victim is the key
/// untouched longest.
template <typename Key>
class LruEvictionPolicy final : public EvictionPolicy<Key> {
   public:
    void touch(const Key& key) override {
        auto it = pos_.find(key);
        if (it != pos_.end()) {
            order_.splice(order_.begin(), order_, it->second);
        } else {
            order_.push_front(key);
            pos_.emplace(key, order_.begin());
        }
    }

    void remove(const Key& key) override {
        auto it = pos_.find(key);
        if (it == pos_.end()) return;
        order_.erase(it->second);
        pos_.erase(it);
    }

    std::optional<Key> pop_victim() override {
        if (order_.empty()) return std::nullopt;
        Key victim = order_.back();
        order_.pop_back();
        pos_.erase(victim);
        return victim;
    }

    std::size_t size() const override { return pos_.size(); }

   private:
    std::list<Key> order_;  // front = most recently used
    std::unordered_map<Key, typename std::list<Key>::iterator> pos_;
};

}  // namespace dftracer::utils::cache

#endif  // DFTRACER_UTILS_DSTD_CACHE_EVICTION_POLICIES_LRU_EVICTION_POLICY_H
