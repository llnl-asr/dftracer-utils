#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/big_endian.h>
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

// The only code that reads or writes the aggregation extension's storage: the
// chunk partials, the per-file chunk column blocks, the per-file dictionary
// entries and the index-wide records (config, time bounds). Reads go
// through an IndexDatabase, writes through an IndexWrite, so the tier runs on
// whichever backend holds the index.
namespace dftracer::utils::index::schemas::dft::agg::tier {

using index::store::IndexDatabase;
using index::store::IndexWrite;

/// What every partial of the index was built with.
struct Config {
    std::uint64_t time_interval_us = 0;
    /// AggregationConfig::params_hash of the build.
    std::uint64_t params_hash = 0;
    /// Consumers that read fhash off a key check this before concluding the
    /// trace touched no files.
    bool group_by_file = true;
    /// AggregationConfig::extra_group_keys of the build: the partials hold
    /// their values, joined by 0x1F in this order.
    std::vector<std::string> extra_group_keys;
};

struct TimeBounds {
    std::uint64_t min_bucket = 0;
    std::uint64_t max_bucket = 0;
};

/// nullopt when the index holds no config, or one in another format.
std::optional<Config> read_config(const IndexDatabase& db);
void write_config(IndexDatabase& db, const Config& config);

/// Marks the partials of `file_id` as built, in the write that carries them.
void put_file(IndexWrite& w, int file_id, std::uint64_t params_hash);
/// As put_file, for files whose partials were written another way.
void put_files(IndexDatabase& db, std::span<const int> file_ids,
               std::uint64_t params_hash);

/// Calls fn(chunk, cat_id, name_id, host_id, extra_id, kind, bytes) for each
/// partial of `file_id` in chunk order until fn returns false; `bytes` is the
/// serialized AggPartial, valid during the call. The ids are the index
/// dictionary ids the file's partials use; the host id is that of the event's
/// hhash text and the extra id that of the extra group key values, each the
/// empty text when there is none. Throws on a read error or a malformed
/// record.
void for_each_partial(
    const IndexDatabase& db, int file_id,
    const std::function<bool(std::uint64_t, std::uint32_t, std::uint32_t,
                             std::uint32_t, std::uint32_t, PartialKind,
                             std::span<const std::uint8_t>)>& fn);

/// Calls fn(chunk, block) for each column block of `file_id` in chunk order
/// until fn returns false. `block.texts` then holds the text of each id of
/// `block.ids`, resolved through the file's dictionary entries (empty where
/// the file holds none), so `block.texts[block.cat[r]]` is the category text of
/// row r and `block.texts[column.path]` the arg path of a column. Every value
/// equals the one the record held: an Int64 or Uint64 value is the same integer
/// of the same variant kind, and a Double value is the same double bit for bit.
/// Throws on a read error or a malformed record.
void for_each_chunk_columns(
    const IndexDatabase& db, int file_id,
    const std::function<bool(std::uint64_t, const ChunkColumns&)>& fn);

/// The column block of `chunk` of `file_id`, with its `chunk` and the texts of
/// its ids filled as for_each_chunk_columns fills them. False, leaving `out`
/// alone, when the index holds no block for the chunk. Throws on a read error
/// or a malformed record.
bool get_chunk_columns(const IndexDatabase& db, int file_id,
                       std::uint64_t chunk, ChunkColumns& out);

std::optional<TimeBounds> read_time_bounds(const IndexDatabase& db);
/// Stores the union of `bounds` and the stored bounds.
void widen_time_bounds(IndexDatabase& db, TimeBounds bounds);

void load_dictionary(const IndexDatabase& db, AggInternTable& table);

/// The index dictionary read once, for a reader that looks up many ids: one
/// scan per index instead of one read per id.
class Dictionary {
   public:
    explicit Dictionary(const IndexDatabase& db);

    /// The text `id` stands for; nullopt when the index holds none.
    const std::string* find(std::uint32_t id) const;

   private:
    ankerl::unordered_dense::map<std::uint32_t, std::string> entries_;
};

/// The string the index dictionary holds for `id`; nullopt when it has none.
std::optional<std::string> read_dictionary_entry(const IndexDatabase& db,
                                                 std::uint32_t id);

/// Removes every partial, chunk column block, dictionary entry and manifest
/// entry of the index, and its index-wide records.
void clear(IndexDatabase& db);

}  // namespace dftracer::utils::index::schemas::dft::agg::tier

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H
