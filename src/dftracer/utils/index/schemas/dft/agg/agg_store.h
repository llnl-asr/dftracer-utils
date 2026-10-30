#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/chunk_columns.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/layout.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The only code that reads or writes the aggregation extension's storage.
namespace dftracer::utils::index::schemas::dft::agg::tier {

using index::store::IndexDatabase;
using index::store::IndexWrite;

struct Config {
    std::uint64_t time_interval_us = 0;
    /// AggregationConfig::params_hash of the build.
    std::uint64_t params_hash = 0;
    /// Consumers that read fhash off a key check this before concluding the
    /// trace touched no files.
    bool group_by_file = true;
    /// The partials hold their values, joined by 0x1F in this order.
    std::vector<std::string> extra_group_keys;
};

struct TimeBounds {
    std::uint64_t min_bucket = 0;
    std::uint64_t max_bucket = 0;
};

/// nullopt when the index holds no config, or one in another format.
std::optional<Config> read_config(const IndexDatabase& db);
void write_config(IndexDatabase& db, const Config& config);

void put_file(IndexWrite& w, int file_id, std::uint64_t params_hash);
void put_files(IndexDatabase& db, std::span<const int> file_ids,
               std::uint64_t params_hash);

/// Calls fn(chunk, cat_id, name_id, host_id, extra_id, kind, bytes) for each
/// partial of `file_id` in chunk order until fn returns false. `bytes` is the
/// serialized AggPartial, valid during the call. A host or extra id is the
/// empty text when there is none. Throws on a read error or a malformed record.
void for_each_partial(
    const IndexDatabase& db, int file_id,
    const std::function<bool(std::uint64_t, std::uint32_t, std::uint32_t,
                             std::uint32_t, std::uint32_t, PartialKind,
                             std::span<const std::uint8_t>)>& fn);

/// Calls fn(chunk, block) for each column block of `file_id` in chunk order
/// until fn returns false. `block.texts` holds the text of each id of
/// `block.ids` (empty where the file holds none). Throws on a read error or a
/// malformed record.
void for_each_chunk_columns(
    const IndexDatabase& db, int file_id,
    const std::function<bool(std::uint64_t, const ChunkColumns&)>& fn);

/// The column block of `chunk` of `file_id`, with its texts filled as
/// for_each_chunk_columns fills them. False, leaving `out` alone, when the
/// index holds no block for the chunk. Throws on a read error or a malformed
/// record.
bool get_chunk_columns(const IndexDatabase& db, int file_id,
                       std::uint64_t chunk, ChunkColumns& out);

std::optional<TimeBounds> read_time_bounds(const IndexDatabase& db);
void widen_time_bounds(IndexDatabase& db, TimeBounds bounds);

void load_dictionary(const IndexDatabase& db, AggInternTable& table);

/// Adds the dictionary entries `file_id` wrote, one for each id its records
/// use, to `texts`. Throws on a read error or a malformed entry.
void load_file_texts(const IndexDatabase& db, int file_id,
                     dstd::HashMap<std::uint32_t, dstd::String>& texts);

/// Removes every record of the aggregation tier from the index.
void clear(IndexDatabase& db);

}  // namespace dftracer::utils::index::schemas::dft::agg::tier

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H
