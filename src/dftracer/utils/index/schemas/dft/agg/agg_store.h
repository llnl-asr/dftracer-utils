#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_serialization.h>
#include <dftracer/utils/index/store/database.h>
#include <rocksdb/iterator.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace dftracer::utils::index::store {
class IndexWrite;
}

// The only code that reads or writes the aggregation and system_metrics
// column families: the dftracer.agg extension's storage. Data rows are keyed
// by a big-endian shard below AGG_KEY_NUM_SHARDS, so their first byte is at
// most 0x0F; the tier's own records (config, dictionary, time bounds, observed
// columns, tracker) sort after every row.
namespace dftracer::utils::index::schemas::dft::agg::tier {

using index::store::RocksDatabase;

/// What every row of the tier was built with.
struct Config {
    std::uint64_t time_interval_us = 0;
    /// AggregationConfig::params_hash of the build.
    std::uint64_t params_hash = 0;
    /// Consumers that read fhash off the key check this before concluding the
    /// trace touched no files.
    bool group_by_file = true;
};

struct TimeBounds {
    std::uint64_t min_bucket = 0;
    std::uint64_t max_bucket = 0;
};

/// Opens `index_path` with the tier's merge operators. A ReadWrite open first
/// closes the process's other handles on the path.
std::shared_ptr<RocksDatabase> open(const std::string& index_path,
                                    RocksDatabase::OpenMode mode);

/// nullopt when the tier holds no config, or one in another format.
std::optional<Config> read_config(const RocksDatabase& db);
void write_config(RocksDatabase& db, const Config& config);

/// Marks the rows of `file_id` as merged into the tier, in the write that
/// carries them.
void put_file(index::store::IndexWrite& w, int file_id,
              std::uint64_t params_hash);
/// As put_file, for files whose rows were written another way.
void put_files(RocksDatabase& db, std::span<const int> file_ids,
               std::uint64_t params_hash);

void merge_row(index::store::IndexWrite& w, std::string_view key,
               std::string_view value);
void merge_system_row(index::store::IndexWrite& w, std::string_view key,
                      std::string_view value);

/// Batched direct writes, committed every few thousand rows and on finish.
class Writer {
   public:
    explicit Writer(RocksDatabase& db);
    void merge_row(std::string_view key, std::string_view value);
    void merge_system_row(std::string_view key, std::string_view value);
    void finish();

   private:
    void count();
    RocksDatabase* db_;
    RocksDatabase::Batch batch_;
    std::size_t pending_ = 0;
};

std::unique_ptr<::rocksdb::Iterator> row_iterator(const RocksDatabase& db);
std::unique_ptr<::rocksdb::Iterator> system_row_iterator(
    const RocksDatabase& db);

/// Calls fn(key, value) for each row of shards [begin, end) in key order
/// until fn returns false; returns the rows visited. Throws on a read error.
template <class Fn>
std::size_t for_each_in(std::unique_ptr<::rocksdb::Iterator> it,
                        std::uint16_t begin, std::uint16_t end, Fn&& fn) {
    const char seek[2] = {static_cast<char>(begin >> 8),
                          static_cast<char>(begin & 0xFF)};
    std::size_t n = 0;
    for (it->Seek(::rocksdb::Slice(seek, 2)); it->Valid(); it->Next()) {
        const auto k = it->key();
        if (k.size() < 2) continue;
        const auto shard =
            static_cast<std::uint16_t>((static_cast<std::uint8_t>(k[0]) << 8) |
                                       static_cast<std::uint8_t>(k[1]));
        if (shard >= end || shard >= AGG_KEY_NUM_SHARDS) break;
        ++n;
        const auto v = it->value();
        if (!fn(std::string_view(k.data(), k.size()),
                std::string_view(v.data(), v.size())))
            break;
    }
    if (!it->status().ok())
        throw DFTUtilsException::cat(
            ErrorCode::IO, "aggregation tier scan: ", it->status().ToString());
    return n;
}

template <class Fn>
std::size_t for_each_row(const RocksDatabase& db, std::uint16_t begin,
                         std::uint16_t end, Fn&& fn) {
    return for_each_in(row_iterator(db), begin, end, std::forward<Fn>(fn));
}

template <class Fn>
std::size_t for_each_system_row(const RocksDatabase& db, std::uint16_t begin,
                                std::uint16_t end, Fn&& fn) {
    return for_each_in(system_row_iterator(db), begin, end,
                       std::forward<Fn>(fn));
}

std::optional<TimeBounds> read_time_bounds(const RocksDatabase& db);
/// Stores the union of `bounds` and the stored bounds.
void widen_time_bounds(RocksDatabase& db, TimeBounds bounds);

std::optional<std::string> read_observed_columns(const RocksDatabase& db);
void write_observed_columns(RocksDatabase& db, std::string_view bytes);
std::optional<std::string> read_tracker(const RocksDatabase& db);
void write_tracker(RocksDatabase& db, std::string_view bytes);

void load_dictionary(const RocksDatabase& db, AggInternTable& table);
/// Writes the entries of `table` added since its last flush.
void flush_dictionary(index::store::IndexWrite& w, AggInternTable& table);
void flush_dictionary(RocksDatabase& db, AggInternTable& table);

/// Removes every row, tier record and dftracer.agg manifest entry of the
/// index, and marks the index's shared intern table unflushed.
void clear(RocksDatabase& db);
/// Folds merge operands into values, so a reader without the merge operators
/// sees whole rows.
void compact(RocksDatabase& db);

}  // namespace dftracer::utils::index::schemas::dft::agg::tier

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H
