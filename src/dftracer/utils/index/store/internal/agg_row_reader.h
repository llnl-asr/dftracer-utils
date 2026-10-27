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

/// Reads the aggregation extension's records on whichever backend holds the
/// index, so the aggregation tier never names a backend. IndexDatabase's
/// friend: the records are read by their index keys, which the typed query
/// API does not expose.
class AggRowReader {
   public:
    /// A record of the aggregation extension: its full index key and its
    /// payload, the value without the header of its (ext, kind).
    using RowFn =
        std::function<bool(std::string_view key, std::string_view body)>;

    /// Calls fn(full key, payload) for every record of (Ext::Agg, kind) under
    /// `prefix`, in key order, until fn returns false. Records whose header
    /// names another type or version are left out. Throws on a read error.
    static void for_each_row(const IndexDatabase& db, std::uint8_t kind,
                             std::string_view prefix, const RowFn& fn);

    /// The payload at the full key `key` of (Ext::Agg, kind); nullopt when it
    /// is absent or its header names another type or version.
    static std::optional<std::string> get_row(const IndexDatabase& db,
                                              std::uint8_t kind,
                                              std::string_view key);

    /// The file ids the index holds aggregation data for, ascending.
    static std::vector<std::uint32_t> file_ids(const IndexDatabase& db);
};

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_AGG_ROW_READER_H
