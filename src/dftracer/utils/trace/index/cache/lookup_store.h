// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_CACHE_LOOKUP_STORE_H
#define DFTRACER_UTILS_TRACE_INDEX_CACHE_LOOKUP_STORE_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/cache/cache_store.h>

#include <cstdint>

namespace dftracer::utils::trace::views::detail {
struct ViewPlan;
}  // namespace dftracer::utils::trace::views::detail

namespace dftracer::utils::trace::index::cache {

/// Where `plan`'s lookup sides live: `cache_dir(plan)/lookups`. Empty when
/// there is none.
dstd::String lookup_cache_path(const trace::views::detail::ViewPlan& plan);

/// A lookup side's identity: a hash of `side_text` (the side's canonical
/// text), the duql version, the record schema params hash, and each file's
/// index registry record (content hash, mtime, size). Nullopt when a file is
/// not fresh in its index, so the side must not be cached. The plan's record
/// schema must be resolved (plan_record_schema); an unresolved one ends the
/// process.
dstd::Optional<std::uint64_t> lookup_signature(
    const trace::views::detail::ViewPlan& plan, dstd::StringView side_text);

/// Open (or reuse) the lookup store at `path`. A ReadWrite open creates the
/// folder. Null when the store cannot be opened, or for a ReadOnly open of a
/// folder with no store. Never throws.
dstd::SharedPtr<CacheStore> open_lookup_db(dstd::StringView path,
                                           CacheStore::Mode mode);

/// The value stored for `sig`, or nullopt. Marks it used when `db` is
/// writable. An Io error on a failed read or usage write.
dstd::Result<dstd::Optional<dstd::String>> read_lookup(CacheStore& db,
                                                       std::uint64_t sig);

/// True if a value for `sig` is stored. No side effect. An Io error on a
/// failed read.
dstd::Result<bool> lookup_exists(const CacheStore& db, std::uint64_t sig);

/// Store `value` for `sig`, then evict the least recently used entries until
/// the store holds at most cache_max_bytes(); a value larger than the budget
/// alone is not kept. An Io error on a failed write.
dstd::Result<void> persist_lookup(CacheStore& db, std::uint64_t sig,
                                  dstd::StringView value);

}  // namespace dftracer::utils::trace::index::cache

#endif  // DFTRACER_UTILS_TRACE_INDEX_CACHE_LOOKUP_STORE_H
