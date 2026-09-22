#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_COUNT_MAP_SCAN_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_COUNT_MAP_SCAN_H

#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/utilities/indexer/internal/payload_codec.h>
#include <dftracer/utils/utilities/indexer/internal/scan_prefix.h>

#include <string_view>
#include <utility>

namespace dftracer::utils::utilities::indexer::internal {

// Invoke `callback(key, count)` for each (name, count) entry of an encoded
// count-map value. The key is a view into `value`, valid only for the call.
template <typename Callback>
void for_each_count_map_entry(std::string_view value, Callback&& callback) {
    Cursor cursor(value);
    auto num_entries = cursor.u32();
    for (std::uint32_t i = 0; i < num_entries; ++i) {
        auto key = cursor.str_view();
        auto count = cursor.u64();
        callback(key, count);
    }
}

// Invoke `callback(key, count)` for each entry of an encoded name-summary
// value, skipping the leading other_count/unique_count header fields.
template <typename Callback>
void for_each_name_summary_entry(std::string_view value, Callback&& callback) {
    Cursor cursor(value);
    auto num_entries = cursor.u32();
    (void)cursor.u64();  // other_count
    (void)cursor.u64();  // unique_count
    for (std::uint32_t i = 0; i < num_entries; ++i) {
        auto key = cursor.str_view();
        auto count = cursor.u64();
        callback(key, count);
    }
}

// Iterate every key/value in `column_family` whose key starts with `prefix`,
// invoking `fn(::rocksdb::Iterator&)` for each; throws on an iterator error.
template <typename Fn>
void scan_prefix(const dftracer::utils::rocksdb::RocksDatabase& db,
                 std::string_view column_family, std::string_view prefix,
                 Fn&& fn) {
    scan_prefix_iterator(
        "Failed to scan RocksDB prefix", prefix,
        [&] { return db.new_iterator(column_family); }, std::forward<Fn>(fn));
}

}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_COUNT_MAP_SCAN_H
