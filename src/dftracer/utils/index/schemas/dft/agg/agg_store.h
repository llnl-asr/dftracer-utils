#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics_serialization.h>
#include <dftracer/utils/index/store/column_families.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/internal/scan_prefix.h>
#include <dftracer/utils/index/store/layout.h>
#include <rocksdb/iterator.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::store {
class IndexWrite;
}

// The only code that reads or writes the aggregation extension's storage: the
// chunk partials, the index-wide records (config, dictionary, tracker, time
// bounds; the blob family) and the system_metrics column family. System rows
// are keyed by a big-endian shard below AGG_KEY_NUM_SHARDS.
namespace dftracer::utils::index::schemas::dft::agg::tier {

using index::store::RocksDatabase;

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

/// Opens `index_path` with the system metrics merge operator. A ReadWrite
/// open first closes the process's other handles on the path.
std::shared_ptr<RocksDatabase> open(const std::string& index_path,
                                    RocksDatabase::OpenMode mode);

/// nullopt when the index holds no config, or one in another format.
std::optional<Config> read_config(const RocksDatabase& db);
void write_config(RocksDatabase& db, const Config& config);

/// Marks the partials of `file_id` as built, in the write that carries them.
void put_file(index::store::IndexWrite& w, int file_id,
              std::uint64_t params_hash);
/// As put_file, for files whose partials were written another way.
void put_files(RocksDatabase& db, std::span<const int> file_ids,
               std::uint64_t params_hash);

void merge_system_row(index::store::IndexWrite& w, std::string_view key,
                      std::string_view value);

/// Batched direct writes of system rows, committed every few thousand rows
/// and on finish.
class Writer {
   public:
    explicit Writer(RocksDatabase& db);
    void merge_system_row(std::string_view key, std::string_view value);
    void finish();

   private:
    RocksDatabase* db_;
    RocksDatabase::Batch batch_;
    std::size_t pending_ = 0;
};

std::unique_ptr<::rocksdb::Iterator> system_row_iterator(
    const RocksDatabase& db);

/// Calls fn(key, value) for each row of shards [begin, end) in key order
/// until fn returns false; returns the rows visited. A non-empty `after`
/// resumes right after that key. Throws on a read error.
template <class Fn>
std::size_t for_each_in(std::unique_ptr<::rocksdb::Iterator> it,
                        std::uint16_t begin, std::uint16_t end, Fn&& fn,
                        std::string_view after = {}) {
    std::uint8_t seek[2];
    write_u16_be(seek, begin);
    std::size_t n = 0;
    const std::string_view start =
        after.empty() ? std::string_view(reinterpret_cast<const char*>(seek), 2)
                      : after;
    index::store::internal::scan_from(
        "aggregation tier scan", std::move(it), start,
        [&](::rocksdb::Iterator& at) {
            const auto k = at.key();
            if (!after.empty() &&
                k == ::rocksdb::Slice(after.data(), after.size()))
                return true;
            if (k.size() < 2) return true;
            const auto shard =
                read_u16_be(reinterpret_cast<const std::uint8_t*>(k.data()));
            if (shard >= end || shard >= AGG_KEY_NUM_SHARDS) return false;
            ++n;
            const auto v = at.value();
            return fn(std::string_view(k.data(), k.size()),
                      std::string_view(v.data(), v.size()));
        });
    return n;
}

template <class Fn>
std::size_t for_each_system_row(const RocksDatabase& db, std::uint16_t begin,
                                std::uint16_t end, Fn&& fn) {
    return for_each_in(system_row_iterator(db), begin, end,
                       std::forward<Fn>(fn));
}

/// Calls fn(chunk, cat_id, name_id, host_id, extra_id, kind, bytes) for each
/// partial of
/// `file_id` in chunk order until fn returns false; `bytes` is the serialized
/// AggPartial, valid during the call. The ids are the index root's dictionary
/// ids; the host id is that of the event's hhash text and the extra id that of
/// the extra group key values, each the empty text when there is none. Throws
/// on a read error or a malformed record.
template <class Fn>
void for_each_partial(const RocksDatabase& db, int file_id, Fn&& fn) {
    const auto prefix = index::store::layout::file_prefix(
        index::store::layout::Ext::Agg, index::store::layout::agg_kind::PARTIAL,
        static_cast<std::uint32_t>(file_id));
    index::store::internal::scan_from(
        "aggregation partial scan", db.new_iterator(index::store::cf::BLOB),
        prefix, [&](::rocksdb::Iterator& it) {
            const std::string_view key(it.key().data(), it.key().size());
            if (!key.starts_with(prefix)) return false;
            const auto value =
                std::string_view(it.value().data(), it.value().size());
            const auto body = index::store::layout::payload(
                value, index::store::layout::Ext::Agg,
                index::store::layout::agg_kind::PARTIAL);
            if (key.size() != prefix.size() + 21 || !body)
                throw DFTUtilsException(
                    ErrorCode::Parse, "aggregation partial: malformed record");
            const auto tail = key.substr(prefix.size());
            return fn(
                index::store::layout::read_u32(tail),
                index::store::layout::read_u32(tail.substr(4)),
                index::store::layout::read_u32(tail.substr(8)),
                index::store::layout::read_u32(tail.substr(12)),
                index::store::layout::read_u32(tail.substr(16)),
                static_cast<PartialKind>(static_cast<std::uint8_t>(tail[20])),
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(body->data()),
                    body->size()));
        });
}

std::optional<TimeBounds> read_time_bounds(const RocksDatabase& db);
/// Stores the union of `bounds` and the stored bounds.
void widen_time_bounds(RocksDatabase& db, TimeBounds bounds);

std::optional<std::string> read_tracker(const RocksDatabase& db);
void write_tracker(RocksDatabase& db, std::string_view bytes);

void load_dictionary(const RocksDatabase& db, AggInternTable& table);
/// The string the dictionary holds for `id`; nullopt when it has none.
std::optional<std::string> read_dictionary_entry(const RocksDatabase& db,
                                                 std::uint32_t id);
/// Writes the entries of `table` added since its last flush.
void flush_dictionary(index::store::IndexWrite& w, AggInternTable& table);
void flush_dictionary(RocksDatabase& db, AggInternTable& table);

/// Removes every partial, index-wide record, system row and dftracer.agg
/// manifest entry of the index, and marks the index's shared intern table
/// unflushed.
void clear(RocksDatabase& db);
/// Folds system metrics merge operands into values, so a reader without the
/// merge operator sees whole rows.
void compact(RocksDatabase& db);

}  // namespace dftracer::utils::index::schemas::dft::agg::tier

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H
