#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics_merge_operator.h>
#include <dftracer/utils/index/store/column_families.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/layout.h>
#include <rocksdb/table.h>

#include <algorithm>

namespace dftracer::utils::index::schemas::dft::agg::tier {

namespace cf = index::store::cf;
namespace layout = index::store::layout;

namespace {

constexpr std::string_view CONFIG_NAME = "config";
constexpr std::string_view TIME_BOUNDS_NAME = "time_bounds";
constexpr std::string_view TRACKER_NAME = "tracker";
constexpr std::size_t CONFIG_FIXED_BYTES = 17;
constexpr char KEY_SEP = '\x1f';
constexpr std::uint8_t FLAG_GROUP_BY_FILE = 1U << 0;
constexpr std::size_t WRITER_BATCH_ROWS = 4096;

// A corrupt index names its directory and says what to do; the status text
// alone does not.
void check(const ::rocksdb::Status& status, const char* what,
           const RocksDatabase& db) {
    if (status.ok()) return;
    if (status.IsCorruption())
        throw DFTUtilsException::cat(
            ErrorCode::Aggregation, what, " failed: the index at ", db.path(),
            " is corrupt (", status.ToString(),
            "); delete that directory and build the index again");
    throw DFTUtilsException::cat(ErrorCode::Io, what, ": ", status.ToString());
}

std::string meta_key(std::string_view name) {
    auto key = layout::file_prefix(layout::Ext::Agg, layout::agg_kind::META,
                                   layout::INDEX_WIDE);
    key.append(name);
    return key;
}

std::string dict_prefix() {
    return layout::file_prefix(layout::Ext::Agg, layout::agg_kind::DICT,
                               layout::INDEX_WIDE);
}

std::string dict_key(std::uint32_t id) {
    auto key = dict_prefix();
    append_u32_be(key, id);
    return key;
}

// The payload of the record `name`; nullopt when it is absent or in another
// format.
std::optional<std::string> get_meta(const RocksDatabase& db,
                                    std::string_view name) {
    std::string value;
    auto status = db.get(meta_key(name), &value, cf::BLOB);
    if (status.IsNotFound()) return std::nullopt;
    check(status, "aggregation record read", db);
    const auto body =
        layout::payload(value, layout::Ext::Agg, layout::agg_kind::META);
    if (!body) return std::nullopt;
    return std::string(*body);
}

void put_meta(RocksDatabase& db, std::string_view name,
              std::string_view value) {
    check(db.put(meta_key(name),
                 layout::with_header(layout::Ext::Agg, layout::agg_kind::META,
                                     value),
                 cf::BLOB),
          "aggregation record write", db);
}

std::string manifest_value(std::uint64_t params_hash) {
    return layout::encode_manifest({layout::ext_version(layout::Ext::Agg),
                                    params_hash, layout::ExtStatus::Ready});
}

template <class Put>
void flush_entries(AggInternTable& table, Put&& put_entry) {
    auto& intern = table.intern;
    const auto current = static_cast<std::uint32_t>(intern.entry_count());
    auto flushed = table.flushed_entries.load(std::memory_order_relaxed);
    if (current <= flushed) return;
    for (std::uint32_t n = flushed; n < current; ++n) {
        const auto id = intern.entry_id(n);
        put_entry(dict_key(id),
                  layout::with_header(layout::Ext::Agg, layout::agg_kind::DICT,
                                      intern.resolve(id)));
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
    auto sys_merge = std::make_shared<SystemMetricsMergeOperator>();
    const bool writing = mode == RocksDatabase::OpenMode::ReadWrite;
    // Rows are rewritten by every rebuild, so a cheap codec keeps flush and
    // compaction CPU low; the rest of the index is tuned for size.
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
    auto override = [sys_merge, writing, fast_compression](
                        const std::string& name,
                        ::rocksdb::ColumnFamilyOptions& opts) {
        if (name == cf::SYSTEM_METRICS) {
            opts.merge_operator = sys_merge;
            if (writing) fast_compression(opts);
        }
    };
    auto& mgr = index::store::RocksDBManager::instance();
    mgr.reset(index_path);
    return mgr.get_or_open(index_path, mode, override);
}

std::optional<Config> read_config(const RocksDatabase& db) {
    auto value = get_meta(db, CONFIG_NAME);
    if (!value || value->size() < CONFIG_FIXED_BYTES) return std::nullopt;
    Config c;
    c.time_interval_us = layout::read_u64(*value);
    c.params_hash = layout::read_u64(std::string_view(*value).substr(8));
    c.group_by_file =
        (static_cast<std::uint8_t>((*value)[16]) & FLAG_GROUP_BY_FILE) != 0;
    // The extra group keys follow, joined by 0x1F.
    std::string_view keys = std::string_view(*value).substr(CONFIG_FIXED_BYTES);
    while (!keys.empty()) {
        const std::size_t at = keys.find(KEY_SEP);
        c.extra_group_keys.emplace_back(keys.substr(0, at));
        if (at == std::string_view::npos) break;
        keys.remove_prefix(at + 1);
    }
    return c;
}

void write_config(RocksDatabase& db, const Config& config) {
    std::string value;
    append_u64_be(value, config.time_interval_us);
    append_u64_be(value, config.params_hash);
    value.push_back(
        static_cast<char>(config.group_by_file ? FLAG_GROUP_BY_FILE : 0));
    for (std::size_t i = 0; i < config.extra_group_keys.size(); ++i) {
        if (i != 0) value.push_back(KEY_SEP);
        value.append(config.extra_group_keys[i]);
    }
    put_meta(db, CONFIG_NAME, value);
}

void put_file(index::store::IndexWrite& w, int file_id,
              std::uint64_t params_hash) {
    index::store::records::put_manifest(w, file_id, layout::Ext::Agg,
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
                                        layout::Ext::Agg),
                   value);
    check(db.commit_batch(batch), "aggregation manifest write", db);
}

void merge_system_row(index::store::IndexWrite& w, std::string_view key,
                      std::string_view value) {
    w.merge(layout::Family::SystemMetrics, key, value);
}

Writer::Writer(RocksDatabase& db) : db_(&db), batch_(db.begin_batch()) {}

void Writer::merge_system_row(std::string_view key, std::string_view value) {
    db_->merge(batch_, cf::SYSTEM_METRICS, key, value);
    if (++pending_ < WRITER_BATCH_ROWS) return;
    check(db_->commit_batch(batch_), "aggregation tier write", *db_);
    batch_ = db_->begin_batch();
    pending_ = 0;
}

void Writer::finish() {
    check(db_->commit_batch(batch_), "aggregation tier write", *db_);
    batch_ = db_->begin_batch();
    pending_ = 0;
}

std::unique_ptr<::rocksdb::Iterator> system_row_iterator(
    const RocksDatabase& db) {
    return db.new_iterator(cf::SYSTEM_METRICS);
}

std::optional<TimeBounds> read_time_bounds(const RocksDatabase& db) {
    auto value = get_meta(db, TIME_BOUNDS_NAME);
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
    append_u64_be(value, bounds.min_bucket);
    append_u64_be(value, bounds.max_bucket);
    put_meta(db, TIME_BOUNDS_NAME, value);
}

std::optional<std::string> read_tracker(const RocksDatabase& db) {
    return get_meta(db, TRACKER_NAME);
}

void write_tracker(RocksDatabase& db, std::string_view bytes) {
    put_meta(db, TRACKER_NAME, bytes);
}

void load_dictionary(const RocksDatabase& db, AggInternTable& table) {
    const auto prefix = dict_prefix();
    index::store::internal::scan_prefix_iterator(
        "aggregation dictionary scan", prefix,
        [&] { return db.new_iterator(cf::BLOB); },
        [&](::rocksdb::Iterator& it) {
            const std::string_view key(it.key().data(), it.key().size());
            const auto body = layout::payload(
                std::string_view(it.value().data(), it.value().size()),
                layout::Ext::Agg, layout::agg_kind::DICT);
            if (key.size() != prefix.size() + 4 || !body)
                throw DFTUtilsException(
                    ErrorCode::Parse,
                    "aggregation dictionary: malformed entry");
            const std::uint32_t id =
                layout::read_u32(key.substr(prefix.size()));
            switch (table.intern.insert_at_id(id, *body)) {
                case dstd::StringIntern::InsertStatus::Ok:
                    break;
                case dstd::StringIntern::InsertStatus::Conflict:
                    throw DFTUtilsException(
                        ErrorCode::InvalidArgument,
                        "string intern: id " + std::to_string(id) +
                            " is already '" +
                            std::string(table.intern.resolve(id)) +
                            "', cannot rebind to '" + std::string(*body) +
                            "' (dictionaries from two indexes in one table?)");
                case dstd::StringIntern::InsertStatus::OutOfRange:
                    throw DFTUtilsException(ErrorCode::InvalidArgument,
                                            "string intern: id " +
                                                std::to_string(id) +
                                                " is out of range");
            }
        });
    table.flushed_entries.store(
        static_cast<std::uint32_t>(table.intern.entry_count()),
        std::memory_order_relaxed);
}

std::optional<std::string> read_dictionary_entry(const RocksDatabase& db,
                                                 std::uint32_t id) {
    std::string value;
    auto status = db.get(dict_key(id), &value, cf::BLOB);
    if (status.IsNotFound()) return std::nullopt;
    check(status, "aggregation dictionary read", db);
    const auto body =
        layout::payload(value, layout::Ext::Agg, layout::agg_kind::DICT);
    if (!body) return std::nullopt;
    return std::string(*body);
}

void flush_dictionary(index::store::IndexWrite& w, AggInternTable& table) {
    flush_entries(table, [&w](std::string_view key, std::string_view value) {
        w.put(layout::Family::Blob, key, value);
    });
}

void flush_dictionary(RocksDatabase& db, AggInternTable& table) {
    auto batch = db.begin_batch();
    flush_entries(table, [&](std::string_view key, std::string_view value) {
        db.put(batch, cf::BLOB, key, value);
    });
    check(db.commit_batch(batch), "aggregation dictionary write", db);
}

void clear(RocksDatabase& db) {
    const std::string all_end(32, '\xFF');
    auto batch = db.begin_batch();
    db.delete_range(batch, cf::SYSTEM_METRICS, std::string_view(), all_end);
    for (const std::uint8_t kind :
         {layout::agg_kind::PARTIAL, layout::agg_kind::META,
          layout::agg_kind::DICT}) {
        const auto begin = layout::kind_prefix(layout::Ext::Agg, kind);
        db.delete_range(batch, cf::BLOB, begin, layout::prefix_end(begin));
    }
    const auto prefix =
        layout::kind_prefix(layout::Ext::Host, layout::host::MANIFEST);
    const std::size_t agg_entry = prefix.size() + 4 + 2;
    index::store::internal::scan_prefix_iterator(
        "aggregation manifest scan", prefix,
        [&] { return db.new_iterator(cf::DEFAULT); },
        [&](::rocksdb::Iterator& it) {
            const std::string_view key(it.key().data(), it.key().size());
            if (key.size() == agg_entry &&
                layout::read_u16(key.substr(prefix.size() + 4)) ==
                    static_cast<std::uint16_t>(layout::Ext::Agg))
                db.del(batch, cf::DEFAULT, key);
        });
    check(db.commit_batch(batch), "aggregation tier clear", db);
    intern_for_index(db.path())->flushed_entries.store(
        0, std::memory_order_relaxed);
}

void compact(RocksDatabase& db) { db.compact(cf::SYSTEM_METRICS); }

}  // namespace dftracer::utils::index::schemas::dft::agg::tier
