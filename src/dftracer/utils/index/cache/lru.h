#ifndef DFTRACER_UTILS_INDEX_CACHE_LRU_H
#define DFTRACER_UTILS_INDEX_CACHE_LRU_H

#include <dftracer/utils/index/store/database.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace dftracer::utils::index::cache {

/// Size of a usage record value: bytes u64, last use u64 (microseconds since
/// the epoch), both big-endian.
inline constexpr std::size_t USAGE_BYTES = 16;

/// Usage record key of `sig`: 0x02 | sig (8 bytes, big-endian).
std::string usage_key(std::uint64_t sig);

/// Stage in `batch` the usage record of `sig` in `cf`: `bytes` held, used now.
void put_usage(index::store::RocksDatabase& db,
               index::store::RocksDatabase::Batch& batch, std::string_view cf,
               std::uint64_t sig, std::uint64_t bytes);

/// Mark `sig` used now. No-op when `db` is read-only or `sig` has no record.
/// Throws IO on a failed write.
void touch_usage(index::store::RocksDatabase& db, std::string_view cf,
                 std::uint64_t sig);

/// Remove the least recently used entries of `cf` until their usage records
/// total at most cache_max_bytes(); `keep` goes only when it alone is over the
/// budget. `drop` stages the removal of an entry's data; the usage record is
/// removed here. Throws IO on a failed scan or write.
void evict_lru(index::store::RocksDatabase& db, std::string_view cf,
               std::uint64_t keep,
               const std::function<void(index::store::RocksDatabase::Batch&,
                                        std::uint64_t)>& drop);

/// Throws DFTUtilsException(IO) with `what` when `st` is not ok.
void check_status(const ::rocksdb::Status& st, const char* what);

}  // namespace dftracer::utils::index::cache

#endif  // DFTRACER_UTILS_INDEX_CACHE_LRU_H
