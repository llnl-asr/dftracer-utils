#ifndef DFTRACER_UTILS_INDEX_STORE_INTERNAL_AGG_ROW_READER_H
#define DFTRACER_UTILS_INDEX_STORE_INTERNAL_AGG_ROW_READER_H

#include <dftracer/utils/index/store/index_database.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::store {

/// Reads the aggregation extension's records by their index keys, which the
/// typed query API does not expose. IndexDatabase's friend.
class AggRowReader {
   public:
    /// The full index key and the payload (the value without its header).
    using RowFn =
        std::function<bool(std::string_view key, std::string_view body)>;

    /// In key order until fn returns false. Records whose header names another
    /// type or version are left out. Throws on a read error.
    static void for_each_row(const IndexDatabase& db, std::uint8_t kind,
                             std::string_view prefix, const RowFn& fn);

    /// nullopt when absent or when the header names another type or version.
    static std::optional<std::string> get_row(const IndexDatabase& db,
                                              std::uint8_t kind,
                                              std::string_view key);
};

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_AGG_ROW_READER_H
