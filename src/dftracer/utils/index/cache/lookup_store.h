#ifndef DFTRACER_UTILS_INDEX_CACHE_LOOKUP_STORE_H
#define DFTRACER_UTILS_INDEX_CACHE_LOOKUP_STORE_H

#include <dftracer/utils/index/store/database.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dftracer::utils::trace::views::detail {
struct ViewPlan;
}  // namespace dftracer::utils::trace::views::detail

namespace dftracer::utils::index::cache {

/// Where `plan`'s lookup sides live: `cache_dir(plan)/lookups`. Empty when
/// there is none.
std::string lookup_cache_path(const trace::views::detail::ViewPlan& plan);

/// A lookup side's identity: a hash of `side_text` (the side's canonical
/// text), the duql version, the record schema params hash, and each file's
/// index registry record (content hash, mtime, size). Nullopt when a file is
/// not fresh in its index, so the side must not be cached. Throws
/// INVALID_ARGUMENT until the plan's record schema is resolved
/// (plan_record_schema).
std::optional<std::uint64_t> lookup_signature(
    const trace::views::detail::ViewPlan& plan, std::string_view side_text);

/// Open (or reuse) the lookup store at `path`. A ReadWrite open creates the
/// folder. Null when the store cannot be opened; never throws.
std::shared_ptr<index::store::RocksDatabase> open_lookup_db(
    const std::string& path, index::store::RocksDatabase::OpenMode mode);

/// The value stored for `sig`, or nullopt. Marks it used when `db` is
/// writable. Throws IO on a failed read or usage write.
std::optional<std::string> read_lookup(index::store::RocksDatabase& db,
                                       std::uint64_t sig);

/// True if a value for `sig` is stored. No side effect.
bool lookup_exists(const index::store::RocksDatabase& db, std::uint64_t sig);

/// Store `value` for `sig`, then evict the least recently used entries until
/// the store holds at most cache_max_bytes(); a value larger than the budget
/// alone is not kept. Throws IO on a failed write.
void persist_lookup(index::store::RocksDatabase& db, std::uint64_t sig,
                    std::string_view value);

}  // namespace dftracer::utils::index::cache

#endif  // DFTRACER_UTILS_INDEX_CACHE_LOOKUP_STORE_H
