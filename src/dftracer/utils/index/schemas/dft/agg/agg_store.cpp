#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_merge_operator.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics_merge_operator.h>
#include <dftracer/utils/index/store/column_families.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/utilities/common/serialization/binary_codec.h>
#include <rocksdb/table.h>

#include <algorithm>

namespace dftracer::utils::index::schemas::dft::agg::tier {

namespace cf = index::store::cf;
namespace layout = index::store::layout;

namespace {

constexpr std::string_view CONFIG_KEY = "\xFF\xFE";
constexpr std::string_view DICT_PREFIX = "\xFF\xFD";
constexpr std::string_view TIME_BOUNDS_KEY = "__time_bounds__";
constexpr std::string_view COLUMNS_KEY = "__observed_columns__";
constexpr std::string_view TRACKER_KEY = "__tracker__";
constexpr std::size_t CONFIG_BYTES = 17;
constexpr std::uint8_t FLAG_GROUP_BY_FILE = 1U << 0;
constexpr std::size_t WRITER_BATCH_ROWS = 4096;

void check(const ::rocksdb::Status& status, const char* what) {
    if (!status.ok())
        throw DFTUtilsException::cat(ErrorCode::IO, what, ": ",
                                     status.ToString());
}

std::optional<std::string> get(const RocksDatabase& db, std::string_view key) {
    std::string value;
    auto status = db.get(key, &value, cf::AGGREGATION);
    if (status.IsNotFound()) return std::nullopt;
    check(status, "aggregation tier read");
    return value;
}

void put(RocksDatabase& db, std::string_view key, std::string_view value) {
    check(db.put(key, value, cf::AGGREGATION), "aggregation tier write");
}

std::string manifest_value(std::uint64_t params_hash) {
    return layout::encode_manifest({layout::ext_version(layout::Ext::AGG),
                                    params_hash, layout::ExtStatus::READY});
}

template <class Put>
void flush_entries(AggInternTable& table, Put&& put_entry) {
    auto& intern = table.intern;
    const auto current = static_cast<std::uint32_t>(intern.entry_count());
    auto flushed = table.flushed_entries.load(std::memory_order_relaxed);
    if (current <= flushed) return;
    std::string key;
    for (std::uint32_t n = flushed; n < current; ++n) {
        const auto id = intern.entry_id(n);
        key.assign(DICT_PREFIX);
        utilities::common::serialization::put_varint(key, id);
        put_entry(key, intern.resolve(id));
    }
    // Another writer may have advanced the watermark meanwhile.
    while (flushed < current &&
           !table.flushed_entries.compare_exchange_weak(
               flushed, current, std::memory_order_relaxed)) {
    }
}

}  // namespace

std::shared_ptr<RocksDatabase> open(const std::string& index_path,
                                    RocksDatabase::OpenMode mode) {
    auto agg_merge = std::make_shared<AggregationMergeOperator>();
    auto sys_merge = std::make_shared<SystemMetricsMergeOperator>();
    const bool writing = mode == RocksDatabase::OpenMode::ReadWrite;
    // Rows are rewritten by every tier rebuild, so a cheap codec keeps flush
    // and compaction CPU low; the rest of the index is tuned for size.
    auto fast_compression = [](::rocksdb::ColumnFamilyOptions& opts) {
#ifdef DFTRACER_UTILS_ENABLE_LZ4
        opts.compression = ::rocksdb::kLZ4Compression;
        opts.bottommost_compression = ::rocksdb::kLZ4Compression;
#elif defined(DFTRACER_UTILS_ENABLE_ZSTD)
        opts.compression = ::rocksdb::kZSTD;
        opts.bottommost_compression = ::rocksdb::kZSTD;
#else
        opts.compression = ::rocksdb::kNoCompression;
        opts.bottommost_compression = ::rocksdb::kNoCompression;
#endif
    };
    auto override = [agg_merge, sys_merge, writing, fast_compression](
                        const std::string& name,
                        ::rocksdb::ColumnFamilyOptions& opts) {
        if (name == cf::AGGREGATION) {
            opts.merge_operator = agg_merge;
            if (!writing) return;
            ::rocksdb::BlockBasedTableOptions bbt;
            bbt.block_size = 32 * 1024;
            bbt.format_version = 7;
            bbt.index_block_restart_interval = 16;
            bbt.whole_key_filtering = false;
            bbt.separate_key_value_in_data_block = true;
            opts.table_factory.reset(::rocksdb::NewBlockBasedTableFactory(bbt));
            opts.level0_file_num_compaction_trigger = 2;
            opts.max_bytes_for_level_multiplier = 20;
            fast_compression(opts);
        } else if (name == cf::SYSTEM_METRICS) {
            opts.merge_operator = sys_merge;
            if (writing) fast_compression(opts);
        }
    };
    auto& mgr = index::store::RocksDBManager::instance();
    mgr.reset(index_path);
    return mgr.get_or_open(index_path, mode, override);
}

std::optional<Config> read_config(const RocksDatabase& db) {
    auto value = get(db, CONFIG_KEY);
    if (!value || value->size() != CONFIG_BYTES) return std::nullopt;
    Config c;
    c.time_interval_us = layout::read_u64(*value);
    c.params_hash = layout::read_u64(std::string_view(*value).substr(8));
    c.group_by_file =
        (static_cast<std::uint8_t>((*value)[16]) & FLAG_GROUP_BY_FILE) != 0;
    return c;
}

void write_config(RocksDatabase& db, const Config& config) {
    std::string value;
    layout::append_u64(value, config.time_interval_us);
    layout::append_u64(value, config.params_hash);
    value.push_back(
        static_cast<char>(config.group_by_file ? FLAG_GROUP_BY_FILE : 0));
    put(db, CONFIG_KEY, value);
}

void put_file(index::store::IndexWrite& w, int file_id,
              std::uint64_t params_hash) {
    index::store::records::put_manifest(w, file_id, layout::Ext::AGG,
                                        params_hash);
}

void put_files(RocksDatabase& db, std::span<const int> file_ids,
               std::uint64_t params_hash) {
    auto batch = db.begin_batch();
    const auto value = manifest_value(params_hash);
    for (int id : file_ids)
        if (id >= 0)
            db.put(batch, cf::DEFAULT,
                   layout::manifest_key(static_cast<std::uint32_t>(id),
                                        layout::Ext::AGG),
                   value);
    check(db.commit_batch(batch), "aggregation manifest write");
}

void merge_row(index::store::IndexWrite& w, std::string_view key,
               std::string_view value) {
    w.merge(layout::Family::AGGREGATION, key, value);
}

void merge_system_row(index::store::IndexWrite& w, std::string_view key,
                      std::string_view value) {
    w.merge(layout::Family::SYSTEM_METRICS, key, value);
}

Writer::Writer(RocksDatabase& db) : db_(&db), batch_(db.begin_batch()) {}

void Writer::count() {
    if (++pending_ < WRITER_BATCH_ROWS) return;
    check(db_->commit_batch(batch_), "aggregation tier write");
    batch_ = db_->begin_batch();
    pending_ = 0;
}

void Writer::merge_row(std::string_view key, std::string_view value) {
    db_->merge(batch_, cf::AGGREGATION, key, value);
    count();
}

void Writer::merge_system_row(std::string_view key, std::string_view value) {
    db_->merge(batch_, cf::SYSTEM_METRICS, key, value);
    count();
}

void Writer::finish() {
    check(db_->commit_batch(batch_), "aggregation tier write");
    batch_ = db_->begin_batch();
    pending_ = 0;
}

std::unique_ptr<::rocksdb::Iterator> row_iterator(const RocksDatabase& db) {
    return db.new_iterator(cf::AGGREGATION);
}

std::unique_ptr<::rocksdb::Iterator> system_row_iterator(
    const RocksDatabase& db) {
    return db.new_iterator(cf::SYSTEM_METRICS);
}

std::optional<TimeBounds> read_time_bounds(const RocksDatabase& db) {
    auto value = get(db, TIME_BOUNDS_KEY);
    if (!value || value->size() < 16) return std::nullopt;
    return TimeBounds{layout::read_u64(*value),
                      layout::read_u64(std::string_view(*value).substr(8))};
}

void widen_time_bounds(RocksDatabase& db, TimeBounds bounds) {
    if (auto stored = read_time_bounds(db)) {
        bounds.min_bucket = std::min(bounds.min_bucket, stored->min_bucket);
        bounds.max_bucket = std::max(bounds.max_bucket, stored->max_bucket);
    }
    std::string value;
    layout::append_u64(value, bounds.min_bucket);
    layout::append_u64(value, bounds.max_bucket);
    put(db, TIME_BOUNDS_KEY, value);
}

std::optional<std::string> read_observed_columns(const RocksDatabase& db) {
    return get(db, COLUMNS_KEY);
}

void write_observed_columns(RocksDatabase& db, std::string_view bytes) {
    put(db, COLUMNS_KEY, bytes);
}

std::optional<std::string> read_tracker(const RocksDatabase& db) {
    return get(db, TRACKER_KEY);
}

void write_tracker(RocksDatabase& db, std::string_view bytes) {
    put(db, TRACKER_KEY, bytes);
}

void load_dictionary(const RocksDatabase& db, AggInternTable& table) {
    auto it = db.new_iterator(cf::AGGREGATION);
    for (it->Seek(::rocksdb::Slice(DICT_PREFIX.data(), DICT_PREFIX.size()));
         it->Valid(); it->Next()) {
        const std::string_view key(it->key().data(), it->key().size());
        if (!key.starts_with(DICT_PREFIX)) break;
        // Key order is not id order past one varint byte, so decode the id.
        utilities::common::serialization::BinaryReader reader(
            key.substr(DICT_PREFIX.size()));
        std::uint32_t id = 0;
        try {
            id = static_cast<std::uint32_t>(reader.varint());
        } catch (const std::exception&) {
            continue;
        }
        table.intern.insert_at_id(
            id, std::string_view(it->value().data(), it->value().size()));
    }
    table.flushed_entries.store(
        static_cast<std::uint32_t>(table.intern.entry_count()),
        std::memory_order_relaxed);
}

void flush_dictionary(index::store::IndexWrite& w, AggInternTable& table) {
    flush_entries(table, [&w](std::string_view key, std::string_view value) {
        w.put(layout::Family::AGGREGATION, key, value);
    });
}

void flush_dictionary(RocksDatabase& db, AggInternTable& table) {
    auto batch = db.begin_batch();
    flush_entries(table, [&](std::string_view key, std::string_view value) {
        db.put(batch, cf::AGGREGATION, key, value);
    });
    check(db.commit_batch(batch), "aggregation dictionary write");
}

void clear(RocksDatabase& db) {
    const std::string all_end(32, '\xFF');
    auto batch = db.begin_batch();
    db.delete_range(batch, cf::AGGREGATION, std::string_view(), all_end);
    db.delete_range(batch, cf::SYSTEM_METRICS, std::string_view(), all_end);
    const auto prefix =
        layout::kind_prefix(layout::Ext::HOST, layout::host::MANIFEST);
    const std::size_t agg_entry = prefix.size() + 4 + 2;
    auto it = db.new_iterator(cf::DEFAULT);
    for (it->Seek(prefix); it->Valid(); it->Next()) {
        const std::string_view key(it->key().data(), it->key().size());
        if (!key.starts_with(prefix)) break;
        if (key.size() == agg_entry &&
            layout::read_u16(key.substr(prefix.size() + 4)) ==
                static_cast<std::uint16_t>(layout::Ext::AGG))
            db.del(batch, cf::DEFAULT, key);
    }
    check(db.commit_batch(batch), "aggregation tier clear");
    intern_for_index(db.path())->flushed_entries.store(
        0, std::memory_order_relaxed);
}

void compact(RocksDatabase& db) {
    db.compact(cf::AGGREGATION);
    db.compact(cf::SYSTEM_METRICS);
}

}  // namespace dftracer::utils::index::schemas::dft::agg::tier
