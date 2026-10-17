#include <dftracer/utils/dstd/cache/eviction_policies/eviction_policy_factory.h>
#include <dftracer/utils/utilities/reader/internal/member_decode_cache.h>

// Compile the eviction policies once for the member cache's key type. This is
// the only place the concrete LRU/CLOCK policies are instantiated, so they
// stay out of every header that uses the cache.
namespace dftracer::utils::cache {
template std::unique_ptr<EvictionPolicy<utilities::reader::internal::MemberKey>>
    make_eviction_policy<utilities::reader::internal::MemberKey>(EvictionKind);
}  // namespace dftracer::utils::cache
