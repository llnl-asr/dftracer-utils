#ifndef DFTRACER_UTILS_DSTD_CACHE_EVICTION_POLICIES_EVICTION_POLICY_FACTORY_H
#define DFTRACER_UTILS_DSTD_CACHE_EVICTION_POLICIES_EVICTION_POLICY_FACTORY_H

#include <dftracer/utils/dstd/cache/eviction_policies/clock_eviction_policy.h>
#include <dftracer/utils/dstd/cache/eviction_policies/lru_eviction_policy.h>
#include <dftracer/utils/dstd/cache/eviction_policy.h>

#include <memory>

namespace dftracer::utils::cache {

// Definition of the make_eviction_policy factory declared in the public
// header. A translation unit that needs it for a concrete Key includes this
// and explicitly instantiates make_eviction_policy<Key>, so the concrete
// policies are compiled once and not exposed to callers.
template <typename Key>
std::unique_ptr<EvictionPolicy<Key>> make_eviction_policy(EvictionKind kind) {
    switch (kind) {
        case EvictionKind::Clock:
            return std::make_unique<ClockEvictionPolicy<Key>>();
        case EvictionKind::Lru:
        default:
            return std::make_unique<LruEvictionPolicy<Key>>();
    }
}

}  // namespace dftracer::utils::cache

#endif  // DFTRACER_UTILS_DSTD_CACHE_EVICTION_POLICIES_EVICTION_POLICY_FACTORY_H
