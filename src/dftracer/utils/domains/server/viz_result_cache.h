// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DOMAINS_SERVER_VIZ_RESULT_CACHE_H
#define DFTRACER_UTILS_DOMAINS_SERVER_VIZ_RESULT_CACHE_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/list.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <mutex>

namespace dftracer::utils::domains::server {

class TraceIndex;

/// Byte-bounded LRU of serialized viz responses in front of the folder's
/// copies in the index store. A memory hit reads no store and allocates no
/// more than the copy it returns. A miss reads the store and keeps the hit in
/// memory; a computed response joins the store under the folder's fingerprint,
/// so a changed or re-indexed folder is a miss. Thread-safe.
class VizResultCache {
   public:
    /// `index` is the folder every stored copy belongs to, and outlives the
    /// cache.
    VizResultCache(std::size_t max_bytes, TraceIndex* index);

    /// The body of `key`, or nothing. A key missing from memory and from the
    /// store is counted as a compute.
    dstd::Optional<dstd::String> get(dstd::StringView key);

    /// The bodies of `keys`, in order: memory first, then one store read for
    /// the keys that missed. A request that finds nothing stored is counted as
    /// one compute, not one per key.
    dstd::Vector<dstd::Optional<dstd::String>> get_many(
        dstd::Span<const dstd::String> keys);

    /// Keeps `body` for `key` in memory at once, then hands it to the folder's
    /// store write. The request does not wait for the store; a store failure
    /// is logged and leaves the memory copy in place.
    void put(dstd::String key, dstd::String body);

   private:
    struct Entry {
        dstd::String body;
        dstd::List<dstd::String>::iterator pos;
    };

    void put_memory(dstd::String key, dstd::String body);
    void evict();  // front = MRU, back = LRU

    TraceIndex* index_;
    std::size_t max_bytes_;
    std::size_t bytes_ = 0;
    dstd::List<dstd::String> order_;
    dstd::HashMap<dstd::String, Entry> map_;
    std::mutex mu_;
};

}  // namespace dftracer::utils::domains::server

#endif  // DFTRACER_UTILS_DOMAINS_SERVER_VIZ_RESULT_CACHE_H
