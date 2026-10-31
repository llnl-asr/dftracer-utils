#ifndef DFTRACER_UTILS_DSTD_CACHE_EVICTION_POLICIES_CLOCK_EVICTION_POLICY_H
#define DFTRACER_UTILS_DSTD_CACHE_EVICTION_POLICIES_CLOCK_EVICTION_POLICY_H

#include <dftracer/utils/dstd/cache/eviction_policy.h>

#include <cstddef>
#include <list>
#include <optional>
#include <unordered_map>

namespace dftracer::utils::cache {

/// CLOCK (second chance): each key carries a reference bit set on touch. The
/// eviction hand sweeps the ring, clearing set bits (a second chance) and
/// evicting the first key whose bit is already clear. Approximates LRU while
/// touch is O(1) with no reordering - cheaper under contention.
template <typename Key>
class ClockEvictionPolicy final : public EvictionPolicy<Key> {
   public:
    void touch(const Key& key) override {
        auto it = pos_.find(key);
        if (it != pos_.end()) {
            it->second->ref = true;
            return;
        }
        ring_.push_back(Slot{key, true});
        pos_.emplace(key, std::prev(ring_.end()));
    }

    void remove(const Key& key) override {
        auto it = pos_.find(key);
        if (it == pos_.end()) return;
        if (hand_ == it->second) ++hand_;  // don't leave the hand dangling
        ring_.erase(it->second);
        pos_.erase(it);
    }

    std::optional<Key> pop_victim() override {
        if (ring_.empty()) return std::nullopt;
        // At most two sweeps: one to clear all set bits, one to find a clear.
        for (std::size_t steps = 0; steps <= 2 * ring_.size(); ++steps) {
            if (hand_ == ring_.end()) hand_ = ring_.begin();
            if (hand_->ref) {
                hand_->ref = false;
                ++hand_;
                continue;
            }
            Key victim = hand_->key;
            pos_.erase(victim);
            hand_ = ring_.erase(hand_);
            return victim;
        }
        return std::nullopt;  // unreachable
    }

    std::size_t size() const override { return pos_.size(); }

   private:
    struct Slot {
        Key key;
        bool ref;
    };
    std::list<Slot> ring_;
    std::unordered_map<Key, typename std::list<Slot>::iterator> pos_;
    typename std::list<Slot>::iterator hand_ = ring_.end();
};

}  // namespace dftracer::utils::cache

#endif  // DFTRACER_UTILS_DSTD_CACHE_EVICTION_POLICIES_CLOCK_EVICTION_POLICY_H
