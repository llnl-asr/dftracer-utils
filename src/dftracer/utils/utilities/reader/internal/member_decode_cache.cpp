// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/cache/eviction_policies/eviction_policy_factory.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/utilities/reader/internal/member_decode_cache.h>

#include <atomic>
#include <mutex>

// Compile the eviction policies once for the member cache's key type. This is
// the only place the concrete LRU/CLOCK policies are instantiated, so they
// stay out of every header that uses the cache.
namespace dftracer::utils::dstd::cache {
// keep std: make_eviction_policy returns a std::unique_ptr
template std::unique_ptr<EvictionPolicy<utilities::reader::internal::MemberKey>>
    make_eviction_policy<utilities::reader::internal::MemberKey>(EvictionKind);
}  // namespace dftracer::utils::dstd::cache

namespace dftracer::utils::utilities::reader::internal {

namespace {

struct GlobalCache {
    std::mutex mutex;
    dstd::Optional<MemberDecodeCache> cache;
    std::atomic<MemberDecodeCache*> published{nullptr};
};

GlobalCache& global_cache() { return dstd::Singleton<GlobalCache>::get(); }

}  // namespace

MemberDecodeCache* global_member_decode_cache() noexcept {
    return global_cache().published.load(std::memory_order_acquire);
}

void configure_global_member_decode_cache(std::size_t capacity_bytes,
                                          dstd::cache::EvictionKind kind) {
    GlobalCache& global = global_cache();
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, [] {
            GlobalCache& held = global_cache();
            std::lock_guard<std::mutex> lock(held.mutex);
            held.published.store(nullptr, std::memory_order_release);
            held.cache.reset();
        });
        return true;
    }();
    std::lock_guard<std::mutex> lock(global.mutex);
    if (!global.cache) global.cache.emplace(capacity_bytes, kind);
    global.published.store(&*global.cache, std::memory_order_release);
}

}  // namespace dftracer::utils::utilities::reader::internal
