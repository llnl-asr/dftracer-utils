#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_BATCH_SCAN_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_BATCH_SCAN_H

#include <dftracer/utils/utilities/indexer/internal/index_encoding.h>
#include <dftracer/utils/utilities/indexer/internal/registry_codec.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>

#include <algorithm>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::utilities::indexer::internal {

// Iterate the rows of `cf` whose decoded file_id is in `wanted`, starting at
// `min_prefix` and stopping once the file_id exceeds `max_file_id`. `per_row`
// is invoked as (int file_id, ::rocksdb::Iterator& it, const std::string& key).
// Returns the iterator status so callers keep their own error handling. The
// prepared form lets sites that scan several column families reuse a single
// `wanted` set without rebuilding it per scan.
template <typename Db, typename Fn>
::rocksdb::Status for_each_file_in_range(Db& db, std::string_view cf,
                                         std::string_view min_prefix,
                                         int max_file_id,
                                         const std::unordered_set<int>& wanted,
                                         Fn&& per_row) {
    auto it = db.new_iterator(cf);
    for (it->Seek(::rocksdb::Slice(min_prefix.data(), min_prefix.size()));
         it->Valid(); it->Next()) {
        auto key = iterator_key(*it);
        int fid = decode_prefixed_file_id(key);
        if (fid > max_file_id) break;
        if (!wanted.contains(fid)) continue;
        per_row(fid, *it, key);
    }
    return it->status();
}

// Convenience wrapper for the common single-scan case: builds `wanted` and the
// min/max bounds from `file_ids`.
template <typename Db, typename Fn>
::rocksdb::Status for_each_file_in_batch(Db& db, std::string_view cf,
                                         const std::vector<int>& file_ids,
                                         Fn&& per_row) {
    if (file_ids.empty()) return ::rocksdb::Status::OK();
    std::unordered_set<int> wanted(file_ids.begin(), file_ids.end());
    const auto [min_it, max_it] =
        std::minmax_element(file_ids.begin(), file_ids.end());
    const auto min_prefix = encoding::prefix_for_file(*min_it);
    return for_each_file_in_range(db, cf, min_prefix, *max_it, wanted,
                                  std::forward<Fn>(per_row));
}

}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_BATCH_SCAN_H
