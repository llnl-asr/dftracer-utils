#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/rocksdb/db_manager.h>
#include <dftracer/utils/core/rocksdb/key_codec.h>
#include <dftracer/utils/trace/aggregators/aggregation_merge_operator.h>
#include <dftracer/utils/trace/aggregators/aggregation_serialization.h>
#include <dftracer/utils/trace/aggregators/association_tracker.h>
#include <dftracer/utils/trace/aggregators/system_metrics_merge_operator.h>
#include <dftracer/utils/utilities/indexer/error.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_sst_writer_context.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/batch_scan.h>
#include <dftracer/utils/utilities/indexer/internal/column_type_codec.h>
#include <dftracer/utils/utilities/indexer/internal/count_map_scan.h>
#include <dftracer/utils/utilities/indexer/internal/db_error.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/index_encoding.h>
#include <dftracer/utils/utilities/indexer/internal/payload_codec.h>
#include <dftracer/utils/utilities/indexer/internal/registry_codec.h>
#include <dftracer/utils/utilities/indexer/internal/scan_prefix.h>
#include <dftracer/utils/utilities/indexer/internal/statistics_codec.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <optional>
#include <shared_mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::indexer {

namespace rocks = dftracer::utils::rocksdb;
namespace cf = rocks::cf;

using namespace internal;

ColumnType merge_column_type(ColumnType a, ColumnType b) {
    if (a == b) return a;
    if (a == ColumnType::Unknown) return b;
    if (b == ColumnType::Unknown) return a;
    if ((a == ColumnType::Int64 && b == ColumnType::Float64) ||
        (a == ColumnType::Float64 && b == ColumnType::Int64))
        return ColumnType::Float64;
    return ColumnType::String;
}

const char* column_type_name(ColumnType t) {
    switch (t) {
        case ColumnType::Int64:
            return "int64";
        case ColumnType::Float64:
            return "float64";
        case ColumnType::String:
            return "string";
        case ColumnType::Unknown:
            return "";
    }
    return "";
}

namespace {

using encoding::prefix_for_file;

// LEB128 varint decode with return-on-truncation policy: if the buffer ends
// mid-varint, return the value accumulated so far (does not throw). Advances
// off past the bytes consumed.
std::string file_bloom_key(int file_id, std::string_view dimension) {
    std::string key = prefix_for_file(file_id);
    key.append(dimension);
    return key;
}

using encoding::metadata_key;
std::string file_scalar_stats_key(int file_id) {
    return prefix_for_file(file_id);
}
std::string file_category_counts_key(int file_id) {
    return prefix_for_file(file_id);
}
std::string file_pid_tid_counts_key(int file_id) {
    return prefix_for_file(file_id);
}
std::string file_name_counts_key(int file_id) {
    return prefix_for_file(file_id);
}

using encoding::name_lookup_key;
using encoding::name_reverse_key;

ChunkBloomResult decode_chunk_bloom(std::string_view key,
                                    std::string_view value,
                                    std::size_t prefix_size) {
    ChunkBloomResult result;
    auto checkpoint_pos = key.find('\0', prefix_size);
    if (checkpoint_pos == std::string_view::npos ||
        checkpoint_pos + 1 + 8 > key.size()) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Corrupt chunk bloom key");
    }
    result.checkpoint_idx =
        rocks::KeyCodec::decode_be64(key.substr(checkpoint_pos + 1, 8));
    if (value.size() < 8) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Corrupt chunk bloom value");
    }
    result.num_entries = rocks::KeyCodec::decode_be64(value.substr(0, 8));
    result.bloom_data.assign(value.begin() + 8, value.end());
    return result;
}

FileBloomResult decode_file_bloom(std::string_view value) {
    if (value.size() < 8) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Corrupt file bloom value");
    }
    FileBloomResult result;
    result.num_entries = rocks::KeyCodec::decode_be64(value.substr(0, 8));
    result.bloom_data.assign(value.begin() + 8, value.end());
    return result;
}

ChunkStatistics decode_chunk_statistics_value(std::string_view value) {
    Cursor cursor(value);
    ChunkStatistics stats;
    stats.total_events = cursor.u64();
    stats.min_timestamp_us = cursor.u64();
    stats.max_timestamp_us = cursor.u64();
    stats.duration_sum_us = cursor.i64();
    stats.duration_min_us = cursor.u64();
    stats.duration_max_us = cursor.u64();
    stats.duration_count = cursor.u64();
    stats.duration_m2 = cursor.f64();

    auto duration_sketch = cursor.blob_view();
    if (!duration_sketch.empty()) {
        stats.duration_sketch = common::statistics::DDSketch::deserialize(
            reinterpret_cast<const std::uint8_t*>(duration_sketch.data()),
            duration_sketch.size());
    }

    auto duration_histogram = cursor.str();
    if (!duration_histogram.empty()) {
        stats.duration_histogram =
            common::statistics::Log2Histogram::from_json(duration_histogram);
    }

    auto name_sketches = cursor.blob_view();
    if (!name_sketches.empty()) {
        stats.name_duration_sketches =
            ChunkStatistics::deserialize_name_duration_sketches(
                reinterpret_cast<const std::uint8_t*>(name_sketches.data()),
                name_sketches.size());
    }

    stats.name_duration_histograms =
        ChunkStatistics::parse_histogram_map_json(cursor.str());
    stats.name_duration_sums =
        ChunkStatistics::parse_double_map_json(cursor.str());
    stats.name_duration_sum_sqs =
        ChunkStatistics::parse_double_map_json(cursor.str());
    stats.name_category = ChunkStatistics::parse_string_map_json(cursor.str());

    auto ts_hist_blob = cursor.blob_view();
    if (!ts_hist_blob.empty()) {
        stats.timestamp_histogram =
            common::statistics::TimestampHistogram::deserialize(
                reinterpret_cast<const std::uint8_t*>(ts_hist_blob.data()),
                ts_hist_blob.size());
    }

    if (!cursor.eof()) {
        auto count = static_cast<std::size_t>(cursor.u32());
        stats.sub_zonemaps.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            trace::indexing::SubChunkZoneMap z;
            z.event_count = cursor.u64();
            z.min_timestamp_us = cursor.u64();
            z.max_timestamp_us = cursor.u64();
            z.min_duration_us = cursor.u64();
            z.max_duration_us = cursor.u64();
            stats.sub_zonemaps.push_back(z);
        }
    }

    // Per-cat and per-pid duration aggregates (tail; absent in older values).
    if (!cursor.eof()) {
        auto cat_sketches = cursor.blob_view();
        if (!cat_sketches.empty()) {
            stats.cat_duration_sketches =
                ChunkStatistics::deserialize_name_duration_sketches(
                    reinterpret_cast<const std::uint8_t*>(cat_sketches.data()),
                    cat_sketches.size());
        }
        stats.cat_duration_sums =
            ChunkStatistics::parse_double_map_json(cursor.str());

        auto pid_sketches = cursor.blob_view();
        if (!pid_sketches.empty()) {
            stats.pid_duration_sketches =
                ChunkStatistics::deserialize_name_duration_sketches(
                    reinterpret_cast<const std::uint8_t*>(pid_sketches.data()),
                    pid_sketches.size());
        }
        stats.pid_duration_sums =
            ChunkStatistics::parse_double_map_json(cursor.str());
    }

    return stats;
}

GzipMemberRecord decode_gzip_member(std::string_view key,
                                    std::string_view value) {
    if (key.size() != 14) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Corrupt gzip member key");
    }

    GzipMemberRecord member;
    member.member_idx = rocks::KeyCodec::decode_be64(key.substr(6, 8));

    Cursor cursor(value);
    member.c_offset = cursor.u64();
    member.c_size = cursor.u64();
    member.uc_offset = cursor.u64();
    member.uc_size = cursor.u64();
    member.first_line_num = cursor.u64();
    member.last_line_num = cursor.u64();
    return member;
}

ChunkDimensionStatsResult decode_chunk_dimension_stats_value(
    std::string_view key, std::string_view value) {
    ChunkDimensionStatsResult result;
    if (key.size() < 12) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Corrupt chunk dimension stats key");
    }
    result.checkpoint_idx = rocks::KeyCodec::decode_be64(key.substr(4, 8));
    result.dimension = std::string(key.substr(12));

    Cursor cursor(value);
    result.distinct_count = cursor.u64();
    result.min_value = cursor.str();
    result.max_value = cursor.str();
    result.value_type = cursor.str();
    if (cursor.u8() != 0) {
        auto compressed = cursor.blob();
        // Defer decompression
        result.compressed_value_counts.assign(compressed.begin(),
                                              compressed.end());
    }
    return result;
}

StringViewMap<std::uint64_t> decode_count_map_value(std::string_view value) {
    Cursor cursor(value);
    StringViewMap<std::uint64_t> counts;
    auto num_entries = cursor.u32();
    counts.reserve(num_entries);
    for (std::uint32_t i = 0; i < num_entries; ++i) {
        auto key = cursor.str();
        counts.emplace(std::move(key), cursor.u64());
    }
    return counts;
}

NameSummaryResult decode_name_summary_value(std::string_view value) {
    Cursor cursor(value);
    NameSummaryResult result;
    auto num_entries = cursor.u32();
    result.other_count = cursor.u64();
    result.unique_count = cursor.u64();
    result.counts.reserve(num_entries);
    for (std::uint32_t i = 0; i < num_entries; ++i) {
        auto key = cursor.str();
        result.counts.emplace(std::move(key), cursor.u64());
    }
    return result;
}

}  // namespace

namespace {

/// Register merge operators for the AGGREGATION and SYSTEM_METRICS CFs on
/// every IndexDatabase open. Previously these operators were set only on
/// the separate handle returned by EventAggregator::open_with_merge_operator,
/// which meant the main IndexDatabase did NOT know how to combine merge
/// operands. Ingested SSTs from the distributed pipeline rely on the
/// operator being registered on the first opener of a given DB path
/// (RocksDBManager caches one instance per path, so later callers get the
/// same handle with these operators already configured).
::rocksdb::CompressionType select_compression_type() {
#ifdef DFTRACER_UTILS_ENABLE_ZSTD
    return ::rocksdb::kZSTD;
#elif defined(DFTRACER_UTILS_ENABLE_LZ4)
    return ::rocksdb::kLZ4Compression;
#else
    return ::rocksdb::kZlibCompression;
#endif
}

rocks::RocksDatabase::CfOptionsOverride make_aggregation_cf_override() {
    using dftracer::utils::trace::aggregators::AggregationMergeOperator;
    using dftracer::utils::trace::aggregators::SystemMetricsMergeOperator;
    auto agg_merge_op = std::make_shared<AggregationMergeOperator>();
    auto sys_merge_op = std::make_shared<SystemMetricsMergeOperator>();
    return [agg_merge_op, sys_merge_op](const std::string& cf_name,
                                        ::rocksdb::ColumnFamilyOptions& opts) {
        if (cf_name == cf::AGGREGATION) {
            opts.merge_operator = agg_merge_op;
            ::rocksdb::BlockBasedTableOptions bbt;
            bbt.block_size = 32 * 1024;
            bbt.format_version = 7;
            bbt.index_block_restart_interval = 16;
            bbt.whole_key_filtering = false;
            bbt.separate_key_value_in_data_block = true;
            opts.table_factory.reset(::rocksdb::NewBlockBasedTableFactory(bbt));
            opts.level0_file_num_compaction_trigger = 2;
            opts.max_bytes_for_level_multiplier = 20;
            opts.compression = select_compression_type();
            opts.bottommost_compression = select_compression_type();
        } else if (cf_name == cf::SYSTEM_METRICS) {
            opts.merge_operator = sys_merge_op;
            opts.compression = select_compression_type();
            opts.bottommost_compression = select_compression_type();
        }
    };
}

}  // namespace

struct IndexDatabase::Impl {
    std::string db_path_;
    rocks::RocksDatabase::OpenMode open_mode_ =
        rocks::RocksDatabase::OpenMode::ReadWrite;
    std::shared_ptr<rocks::RocksDatabase> db_;

    struct HashCache {
        std::shared_mutex mutex;
        bool loaded = false;
        std::unordered_map<std::string, std::string> file_hash;
        std::unordered_map<std::string, std::string> host_hash;
        std::unordered_map<std::string, std::string> string_hash;
        std::unordered_map<std::string, std::string> proc_hash;
        std::unordered_map<std::string, std::string> file_name;
        std::unordered_map<std::string, std::string> host_name;
        std::unordered_map<std::string, std::string> string_name;
        std::unordered_map<std::string, std::string> proc_name;
    };
    std::unique_ptr<HashCache> hash_cache_;
};

IndexDatabase::IndexDatabase(const std::string& index_path,
                             IndexOpenMode open_mode)
    : impl_(std::make_unique<Impl>()) {
    impl_->db_path_ = internal::normalize_index_root(index_path);
    impl_->open_mode_ = open_mode == IndexOpenMode::ReadOnly
                            ? rocks::RocksDatabase::OpenMode::ReadOnly
                            : rocks::RocksDatabase::OpenMode::ReadWrite;
    impl_->db_ = rocks::RocksDBManager::instance().get_or_open(
        impl_->db_path_, impl_->open_mode_, make_aggregation_cf_override());
    if (impl_->open_mode_ == rocks::RocksDatabase::OpenMode::ReadWrite) {
        init_schema();
    }
}

IndexDatabase::IndexDatabase(IndexDatabase&&) noexcept = default;
IndexDatabase& IndexDatabase::operator=(IndexDatabase&&) noexcept = default;
IndexDatabase::~IndexDatabase() = default;

std::shared_ptr<rocks::RocksDatabase> IndexDatabase::db() const {
    return impl_->db_;
}

std::unique_ptr<IndexDatabaseWriterContext> IndexDatabase::begin_write() {
    return std::unique_ptr<IndexDatabaseWriterContext>(
        new IndexDatabaseWriterContext(impl_->db_));
}

void IndexDatabase::bulk_ingest(
    const SstArtifactRegistry& registry,
    const std::unordered_set<std::string>& skip_cfs) {
    const auto skipped = [&](std::string_view cf_name) {
        return skip_cfs.find(std::string(cf_name)) != skip_cfs.end();
    };

    // One atomic multi-CF ingest: a single manifest edit. Overlapping
    // content-addressed / aggregation SSTs are fine with allow_global_seqno,
    // which assigns seqnos in list order (so the ordered merge still holds).
    std::vector<std::pair<std::string_view, const std::vector<std::string>*>>
        per_cf = {
            {cf::METADATA, &registry.metadata()},
            {cf::MEMBERS, &registry.members()},
            {cf::MANIFEST, &registry.manifest()},
            {cf::CHUNK_BLOOM, &registry.chunk_bloom()},
            {cf::FILE_BLOOM, &registry.file_bloom()},
            {cf::CHUNK_STATS, &registry.chunk_stats()},
            {cf::CHUNK_DIM_STATS, &registry.chunk_dim_stats()},
            {cf::DIMENSIONS, &registry.dimensions()},
            {cf::FILE_SCALAR_STATS, &registry.file_scalar_stats()},
            {cf::FILE_CAT_COUNTS, &registry.file_cat_counts()},
            {cf::FILE_PID_TID_COUNTS, &registry.file_pid_tid_counts()},
            {cf::FILE_NAME_COUNTS, &registry.file_name_counts()},
            {cf::NAME_FILE_POSTINGS, &registry.name_file_postings()},
            {cf::NAME_CHUNK_POSTINGS, &registry.name_chunk_postings()},
            {cf::NAME_DICTIONARY, &registry.name_dictionary()},
            {cf::HASH_TABLES, &registry.hash_tables()},
            {cf::AGGREGATION, &registry.aggregation()},
            {cf::SYSTEM_METRICS, &registry.system_metrics()},
        };
    per_cf.erase(std::remove_if(per_cf.begin(), per_cf.end(),
                                [&](const auto& e) {
                                    return skipped(e.first) ||
                                           e.second->empty();
                                }),
                 per_cf.end());
    if (per_cf.empty()) return;

    auto status = impl_->db_->ingest_external_files_multi(per_cf);
    if (!status.ok()) {
        throw_db_error("Failed to ingest SSTs into column family '" +
                           std::string(per_cf.front().first) + "'",
                       status);
    }
}

void IndexDatabase::rebuild_root_summaries() {
    auto writer = begin_write();
    writer->rebuild_root_summaries();
    writer->commit();
}

void IndexDatabase::write_agg_global_config(std::uint64_t time_interval_us,
                                            std::uint32_t config_hash,
                                            bool group_by_file) {
    using dftracer::utils::trace::aggregators::AGG_GLOBAL_CONFIG_KEY;
    using dftracer::utils::trace::aggregators::AggGlobalConfig;
    using dftracer::utils::trace::aggregators::serialize_agg_global_config;

    AggGlobalConfig cfg;
    cfg.time_interval_us = time_interval_us;
    cfg.config_hash = config_hash;
    cfg.group_by_file = group_by_file;
    auto status =
        impl_->db_->put(std::string_view(AGG_GLOBAL_CONFIG_KEY, 2),
                        serialize_agg_global_config(cfg), cf::AGGREGATION);
    if (!status.ok()) {
        throw_db_error("Failed to write aggregation global config", status);
    }
}

void IndexDatabase::write_aggregation_tracker(
    const std::vector<std::string>& blobs) {
    using dftracer::utils::trace::aggregators::AssociationTracker;

    AssociationTracker unified;
    for (const auto& b : blobs) {
        if (b.empty()) continue;
        unified.merge(AssociationTracker::deserialize(b));
    }
    unified.finalize();
    constexpr std::string_view TRACKER_KEY = "__tracker__";
    auto status =
        impl_->db_->put(TRACKER_KEY, unified.serialize(), cf::AGGREGATION);
    if (!status.ok()) {
        throw_db_error("Failed to write aggregation tracker", status);
    }
}

void IndexDatabase::write_agg_file_markers(const std::vector<int>& file_ids) {
    using dftracer::utils::trace::aggregators::make_agg_file_key;

    auto batch = impl_->db_->begin_batch();
    for (int file_id : file_ids) {
        if (file_id < 0) continue;
        impl_->db_->put(batch, cf::AGGREGATION,
                        make_agg_file_key(static_cast<std::int32_t>(file_id)),
                        "");
    }
    auto status = impl_->db_->commit_batch(batch);
    if (!status.ok()) {
        throw_db_error("Failed to write aggregation file markers", status);
    }
}

std::vector<int> IndexDatabase::register_files(
    const std::vector<std::string>& file_paths) {
    IndexFileEntryCapability caps = IndexFileEntryCapability::BLOOM |
                                    IndexFileEntryCapability::MEMBERS |
                                    IndexFileEntryCapability::FILE_SUMMARY |
                                    IndexFileEntryCapability::INDEXING_COMPLETE;

    std::vector<int> ids;
    ids.reserve(file_paths.size());
    auto writer = begin_write();
    for (const auto& path : file_paths) {
        const auto logical = internal::get_logical_path(path);
        const auto file_hash = internal::calculate_file_hash(path);
        const auto file_mtime = static_cast<std::uint64_t>(
            internal::get_file_modification_time(path));
        const auto file_size = internal::file_size_bytes(path);
        ids.push_back(writer->get_or_create_file_info(logical, file_hash, caps,
                                                      file_mtime, file_size));
    }
    writer->commit();
    return ids;
}

int IndexDatabase::reserve_file_id_range(std::size_t count) {
    if (count == 0) {
        // Return the next id without advancing the counter.
        std::string value;
        const auto key = std::string(encoding::NEXT_FILE_ID_KEY);
        auto status = impl_->db_->get(key, &value);
        if (status.IsNotFound()) return 1;
        if (!status.ok()) {
            throw_db_error("Failed to read next file id", status);
        }
        return static_cast<int>(rocks::KeyCodec::decode_be32(value));
    }

    std::string value;
    const auto key = std::string(encoding::NEXT_FILE_ID_KEY);
    auto status = impl_->db_->get(key, &value);

    std::uint32_t first = 1;
    if (status.ok()) {
        first = rocks::KeyCodec::decode_be32(value);
    } else if (!status.IsNotFound()) {
        throw_db_error("Failed to read next file id", status);
    }

    const std::uint32_t next = first + static_cast<std::uint32_t>(count);
    const auto encoded = rocks::KeyCodec::encode_be32(next);
    auto put_status = impl_->db_->put(key, encoded);
    if (!put_status.ok()) {
        throw_db_error("Failed to advance next file id", put_status);
    }
    return static_cast<int>(first);
}

void IndexDatabase::init_schema() {
    std::string value;
    auto status = impl_->db_->get(schema_version_key(), &value);
    if (status.IsNotFound()) {
        status = impl_->db_->put(
            schema_version_key(),
            rocks::KeyCodec::encode_be32(IndexDatabase::SCHEMA_VERSION));
        if (!status.ok()) {
            throw_db_error("Failed to initialize schema version", status);
        }
    } else if (!status.ok()) {
        throw_db_error("Failed to read schema version", status);
    }
}

bool IndexDatabase::has_bloom_data(int file_id) const {
    auto caps = get_file_capabilities(file_id);
    if (has_capability(caps, IndexFileEntryCapability::BLOOM)) return true;
    bool found = false;
    auto prefix = prefix_for_file(file_id);
    scan_prefix(*impl_->db_, cf::CHUNK_BLOOM, prefix,
                [&found](::rocksdb::Iterator&) { found = true; });
    return found;
}

IndexFileEntryCapability IndexDatabase::get_file_capabilities(
    int file_id) const {
    std::string name;
    auto status = impl_->db_->get(file_reverse_key(file_id), &name);
    if (!status.ok()) return IndexFileEntryCapability::NONE;

    std::string record;
    status = impl_->db_->get(file_lookup_key(name), &record);
    if (!status.ok()) return IndexFileEntryCapability::NONE;

    return decode_file_capabilities(record);
}

int IndexDatabase::get_file_info_id(std::string_view path) const {
    std::string value;
    auto status = impl_->db_->get(file_lookup_key(path), &value);
    if (status.IsNotFound()) {
        return -1;
    }
    if (!status.ok()) {
        throw_db_error("Failed to look up file info id", status);
    }
    return decode_file_id(value);
}

std::optional<std::uint64_t> IndexDatabase::get_file_hash(
    std::string_view path) const {
    std::string value;
    auto status = impl_->db_->get(file_lookup_key(path), &value);
    if (status.IsNotFound()) {
        return std::nullopt;
    }
    if (!status.ok()) {
        throw_db_error("Failed to look up file hash", status);
    }
    return decode_file_hash(value);
}

std::optional<IndexDatabase::FileStat> IndexDatabase::get_file_stat(
    std::string_view path) const {
    std::string value;
    auto status = impl_->db_->get(file_lookup_key(path), &value);
    if (status.IsNotFound()) {
        return std::nullopt;
    }
    if (!status.ok()) {
        throw_db_error("Failed to look up file stat", status);
    }
    auto mtime = internal::decode_file_mtime(value);
    auto size = internal::decode_file_size(value);
    if (!mtime || !size) {
        return std::nullopt;  // pre-v2 record
    }
    return FileStat{*mtime, *size};
}

std::uint32_t IndexDatabase::get_schema_version() const {
    std::string value;
    auto status = impl_->db_->get(schema_version_key(), &value);
    if (status.IsNotFound()) {
        return 0;
    }
    if (!status.ok()) {
        throw_db_error("Failed to read schema version", status);
    }
    return rocks::KeyCodec::decode_be32(value);
}

bool IndexDatabase::schema_outdated() const {
    return get_schema_version() < SCHEMA_VERSION;
}

IndexDatabase::Freshness IndexDatabase::check_freshness(
    const std::string& file_path) const {
    // Stat-only by design: mtime + size are the only O(1) freshness signals,
    // and this runs on every index open and every stale scan, so it must not
    // read the file body. A same-size edit with a deliberately restored mtime
    // is the one change this cannot see; that is accepted rather than pay a
    // whole-file read on the hot path.
    if (schema_outdated()) return Freshness::SchemaOutdated;

    const auto logical = internal::get_logical_path(file_path);
    if (get_file_info_id(logical) < 0) return Freshness::Stale;

    auto stored = get_file_stat(logical);
    if (!stored) return Freshness::Stale;  // record predates stat tracking

    const auto current_mtime = static_cast<std::uint64_t>(
        internal::get_file_modification_time(file_path));
    const auto current_size = internal::file_size_bytes(file_path);
    if (stored->mtime != current_mtime || stored->size != current_size) {
        return Freshness::Stale;
    }
    return Freshness::Fresh;
}

IndexDatabase::StaleCheckResult IndexDatabase::find_stale_files(
    const std::vector<std::string>& current_paths) const {
    StaleCheckResult result;
    result.schema_outdated = schema_outdated();

    std::unordered_set<std::string> seen_logical;
    seen_logical.reserve(current_paths.size());
    for (const auto& path : current_paths) {
        const auto logical = internal::get_logical_path(path);
        seen_logical.insert(logical);
        if (get_file_info_id(logical) < 0) {
            result.added.push_back(path);
            continue;
        }
        if (check_freshness(path) != Freshness::Fresh) {
            result.changed.push_back(path);
        }
    }

    for (const auto& [logical, _] : query_all_file_info_ids()) {
        if (seen_logical.find(logical) == seen_logical.end()) {
            result.removed.push_back(logical);
        }
    }
    return result;
}

std::unordered_map<std::string, int> IndexDatabase::query_all_file_info_ids()
    const {
    std::unordered_map<std::string, int> results;
    internal::scan_prefix_iterator(
        "Failed to scan file registry", "f|",
        [this] { return impl_->db_->new_iterator(); },
        [&](::rocksdb::Iterator& it) {
            auto key = iterator_key(it);
            auto value = iterator_value(it);
            results.emplace(key.substr(2), decode_file_id(value));
        });
    return results;
}

std::unordered_map<std::string, FileRegistryEntry>
IndexDatabase::query_all_file_registry() const {
    std::unordered_map<std::string, FileRegistryEntry> results;
    internal::scan_prefix_iterator(
        "Failed to scan file registry", "f|",
        [this] { return impl_->db_->new_iterator(); },
        [&](::rocksdb::Iterator& it) {
            auto key = iterator_key(it);
            auto value = iterator_value(it);
            FileRegistryEntry entry;
            entry.file_id = decode_file_id(value);
            entry.capabilities = decode_file_capabilities(value);
            results.emplace(key.substr(2), entry);
        });
    return results;
}

int IndexDatabase::find_file(std::string_view file_path) const {
    return get_file_info_id(internal::get_logical_path(file_path));
}

std::optional<std::uint64_t> IndexDatabase::query_name_id(
    std::string_view name) const {
    std::string value;
    auto status =
        impl_->db_->get(name_lookup_key(name), &value, cf::NAME_DICTIONARY);
    if (status.IsNotFound()) {
        return std::nullopt;
    }
    if (!status.ok()) {
        throw_db_error("Failed to query name dictionary", status);
    }
    return rocks::KeyCodec::decode_be64(value);
}

std::optional<std::string> IndexDatabase::query_name_by_id(
    std::uint64_t name_id) const {
    std::string value;
    auto status =
        impl_->db_->get(name_reverse_key(name_id), &value, cf::NAME_DICTIONARY);
    if (status.IsNotFound()) {
        return std::nullopt;
    }
    if (!status.ok()) {
        throw_db_error("Failed to query name reverse dictionary", status);
    }
    return value;
}

bool IndexDatabase::has_file_scalar_stats(int file_id) const {
    std::string value;
    auto status = impl_->db_->get(file_scalar_stats_key(file_id), &value,
                                  cf::FILE_SCALAR_STATS);
    if (status.IsNotFound()) {
        return false;
    }
    if (!status.ok()) {
        throw_db_error("Failed to check file scalar statistics", status);
    }
    return true;
}

std::vector<ChunkBloomResult> IndexDatabase::query_chunk_bloom_filters(
    int file_id, std::string_view dimension) const {
    std::vector<ChunkBloomResult> results;
    std::string prefix = prefix_for_file(file_id);
    prefix.append(dimension);
    prefix.push_back('\0');
    scan_prefix(
        *impl_->db_, cf::CHUNK_BLOOM, prefix, [&](::rocksdb::Iterator& it) {
            results.push_back(decode_chunk_bloom(
                iterator_key(it), iterator_value(it), prefix.size() - 1));
        });
    return results;
}

std::unordered_map<std::string, std::vector<ChunkBloomResult>>
IndexDatabase::query_chunk_bloom_filters_batch(
    int file_id, const std::vector<std::string>& dimensions) const {
    std::unordered_map<std::string, std::vector<ChunkBloomResult>> results;
    for (const auto& dimension : dimensions) {
        results.emplace(dimension,
                        query_chunk_bloom_filters(file_id, dimension));
    }
    return results;
}

std::optional<FileBloomResult> IndexDatabase::query_file_bloom_filter(
    int file_id, std::string_view dimension) const {
    std::string value;
    auto status = impl_->db_->get(file_bloom_key(file_id, dimension), &value,
                                  cf::FILE_BLOOM);
    if (status.IsNotFound()) {
        return std::nullopt;
    }
    if (!status.ok()) {
        throw_db_error("Failed to query file bloom filter", status);
    }
    return decode_file_bloom(value);
}

std::unordered_map<std::string, FileBloomResult>
IndexDatabase::query_file_bloom_filters_batch(
    int file_id, const std::vector<std::string>& dimensions) const {
    std::unordered_map<std::string, FileBloomResult> results;
    for (const auto& dimension : dimensions) {
        auto bloom = query_file_bloom_filter(file_id, dimension);
        if (bloom) {
            results.emplace(dimension, std::move(*bloom));
        }
    }
    return results;
}

std::vector<std::string> IndexDatabase::query_index_dimensions(
    int file_id) const {
    std::vector<std::string> dimensions;
    std::string prefix("d|");
    rocks::KeyCodec::append_be32(prefix, static_cast<std::uint32_t>(file_id));
    scan_prefix(*impl_->db_, cf::DIMENSIONS, prefix,
                [&](::rocksdb::Iterator& it) {
                    auto key = iterator_key(it);
                    dimensions.push_back(key.substr(prefix.size()));
                });
    return dimensions;
}

std::vector<std::string> IndexDatabase::query_all_columns() const {
    // Key layout: "c|" + BE32(file_id) + column. Scan the whole "c|" space and
    // union the column names across files.
    constexpr std::size_t HEADER = 2 + sizeof(std::uint32_t);
    ankerl::unordered_dense::set<std::string> cols;
    scan_prefix(*impl_->db_, cf::DIMENSIONS, "c|",
                [&](::rocksdb::Iterator& it) {
                    auto key = iterator_key(it);
                    if (key.size() > HEADER) cols.emplace(key.substr(HEADER));
                });
    std::vector<std::string> out(cols.begin(), cols.end());
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

// Decodes a "c|" column record value: schema v15+ stores the TLV-encoded
// DataType, a pre-v15 record stores the one-byte ColumnType. An undecodable
// (corrupt) value degrades to Unknown rather than throwing, since this scans
// arbitrary on-disk bytes.
dataframe::DataType decode_column_record(std::string_view value) {
    if (value.empty()) return dataframe::scalar(dataframe::TypeId::Unknown);
    if (value.size() == 1) {
        return column_type_to_data_type(
            static_cast<ColumnType>(static_cast<std::uint8_t>(value[0])));
    }
    if (auto decoded = decode_data_type(value)) return *decoded;
    return dataframe::scalar(dataframe::TypeId::Unknown);
}

}  // namespace

std::vector<std::pair<std::string, ColumnType>>
IndexDatabase::query_all_column_types() const {
    auto typed = query_all_column_data_types();
    std::vector<std::pair<std::string, ColumnType>> out;
    out.reserve(typed.size());
    for (auto& [name, type] : typed)
        out.emplace_back(std::move(name), data_type_to_column_type(type));
    return out;
}

std::vector<std::pair<std::string, dataframe::DataType>>
IndexDatabase::query_all_column_data_types() const {
    // As query_all_columns, but the record value carries the TLV-encoded
    // DataType (schema v15+; a pre-v15 record decodes via the legacy
    // one-byte ColumnType path). Fold the type across files.
    constexpr std::size_t HEADER = 2 + sizeof(std::uint32_t);
    ankerl::unordered_dense::map<std::string, dataframe::DataType> cols;
    scan_prefix(
        *impl_->db_, cf::DIMENSIONS, "c|", [&](::rocksdb::Iterator& it) {
            auto key = iterator_key(it);
            if (key.size() <= HEADER) return;
            dataframe::DataType t = decode_column_record(iterator_value(it));
            std::string name(key.substr(HEADER));
            auto [pos, inserted] = cols.emplace(std::move(name), t);
            if (!inserted) pos->second = merge_data_type(pos->second, t);
        });
    std::vector<std::pair<std::string, dataframe::DataType>> out(cols.begin(),
                                                                 cols.end());
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

std::vector<ChunkStatisticsResult> IndexDatabase::query_chunk_statistics(
    int file_id) const {
    std::vector<ChunkStatisticsResult> results;
    const auto prefix = prefix_for_file(file_id);
    scan_prefix(
        *impl_->db_, cf::CHUNK_STATS, prefix, [&](::rocksdb::Iterator& it) {
            ChunkStatisticsResult result;
            auto key = iterator_key(it);
            result.checkpoint_idx = rocks::KeyCodec::decode_be64(
                std::string_view(key).substr(4, 8));
            result.stats = decode_chunk_statistics_value(iterator_value(it));
            results.push_back(std::move(result));
        });
    std::sort(results.begin(), results.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.checkpoint_idx < rhs.checkpoint_idx;
              });
    return results;
}

std::unordered_map<int, std::vector<ChunkStatisticsResult>>
IndexDatabase::query_chunk_statistics_batch(
    const std::vector<int>& file_ids) const {
    std::unordered_map<int, std::vector<ChunkStatisticsResult>> results;
    if (file_ids.empty()) {
        return results;
    }
    results.reserve(file_ids.size());

    auto status = for_each_file_in_batch(
        *impl_->db_, cf::CHUNK_STATS, file_ids,
        [&](int file_id, ::rocksdb::Iterator& it, const std::string& key) {
            ChunkStatisticsResult result;
            result.checkpoint_idx = rocks::KeyCodec::decode_be64(
                std::string_view(key).substr(4, 8));
            result.stats = decode_chunk_statistics_value(iterator_value(it));
            results[file_id].push_back(std::move(result));
        });
    if (!status.ok()) {
        throw_db_error("Failed to batch query chunk statistics", status);
    }

    for (auto& [_, entries] : results) {
        std::sort(entries.begin(), entries.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return lhs.checkpoint_idx < rhs.checkpoint_idx;
                  });
    }
    return results;
}

std::unordered_map<int, MergedStatisticsResult>
IndexDatabase::query_merged_statistics_batch(
    const std::vector<int>& file_ids) const {
    std::unordered_map<int, MergedStatisticsResult> results;
    if (file_ids.empty()) {
        return results;
    }
    results.reserve(file_ids.size());

    std::unordered_set<int> wanted(file_ids.begin(), file_ids.end());
    const auto [min_it, max_it] =
        std::minmax_element(file_ids.begin(), file_ids.end());
    const auto min_prefix = prefix_for_file(*min_it);
    const int max_file_id = *max_it;

    auto stats_status = for_each_file_in_range(
        *impl_->db_, cf::CHUNK_STATS, min_prefix, max_file_id, wanted,
        [&](int file_id, ::rocksdb::Iterator& it, const std::string&) {
            auto decoded = decode_chunk_statistics_value(iterator_value(it));
            auto& merged = results[file_id];
            if (merged.num_chunks == 0) {
                merged.stats = std::move(decoded);
            } else {
                merged.stats.merge_from(decoded);
            }
            ++merged.num_chunks;
        });
    if (!stats_status.ok()) {
        throw_db_error("Failed to batch merge chunk statistics", stats_status);
    }

    auto dims_status = for_each_file_in_range(
        *impl_->db_, cf::CHUNK_DIM_STATS, min_prefix, max_file_id, wanted,
        [&](int file_id, ::rocksdb::Iterator& it, const std::string& key) {
            auto decoded =
                decode_chunk_dimension_stats_value(key, iterator_value(it));
            if (!decoded.has_value_counts_payload()) return;
            decoded.ensure_value_counts_decoded();
            if (!decoded.value_counts) return;

            auto& merged = results[file_id].stats;
            if (decoded.dimension == "cat") {
                for (const auto& [k, v] : *decoded.value_counts) {
                    merged.category_counts[k] += v;
                }
            } else if (decoded.dimension == "name") {
                for (const auto& [k, v] : *decoded.value_counts) {
                    merged.name_counts[k] += v;
                }
            } else if (decoded.dimension == "pid_tid") {
                for (const auto& [k, v] : *decoded.value_counts) {
                    merged.pid_tid_counts[k] += v;
                }
            }
        });
    if (!dims_status.ok()) {
        throw_db_error("Failed to batch merge chunk dimension stats",
                       dims_status);
    }

    return results;
}

std::unordered_map<int, MergedStatisticsResult>
IndexDatabase::query_file_scalar_stats_batch(
    const std::vector<int>& file_ids) const {
    std::unordered_map<int, MergedStatisticsResult> results;
    results.reserve(file_ids.size());
    for (const auto file_id : file_ids) {
        std::string value;
        auto status = impl_->db_->get(file_scalar_stats_key(file_id), &value,
                                      cf::FILE_SCALAR_STATS);
        if (status.IsNotFound()) {
            continue;
        }
        if (!status.ok()) {
            throw_db_error("Failed to read file scalar statistics", status);
        }
        try {
            DecodeContextGuard ctx("file_scalar_stats file_id=%d size=%zu",
                                   file_id, value.size());
            results.emplace(file_id, decode_file_scalar_stats_value(value));
        } catch (const std::exception& e) {
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Corrupt file_scalar_stats payload file_id=" +
                                   std::to_string(file_id) +
                                   " size=" + std::to_string(value.size()) +
                                   ": " + e.what());
        }
    }
    return results;
}

std::unordered_map<int, FileMetadataResult>
IndexDatabase::query_file_metadata_batch(
    const std::vector<int>& file_ids) const {
    std::unordered_map<int, FileMetadataResult> results;
    if (file_ids.empty()) {
        return results;
    }
    results.reserve(file_ids.size());

    auto status = for_each_file_in_batch(
        *impl_->db_, cf::METADATA, file_ids,
        [&](int file_id, ::rocksdb::Iterator& it, const std::string&) {
            auto value = iterator_value(it);
            DecodeContextGuard ctx("metadata file_id=%d size=%zu", file_id,
                                   value.size());
            auto decoded = decode_metadata_record(value);
            auto& meta = results[file_id];
            meta.checkpoint_size = decoded[0];
            meta.num_lines = decoded[1];
            meta.max_bytes = decoded[2];
        });
    if (!status.ok()) {
        throw_db_error("Failed to batch read file metadata", status);
    }
    return results;
}

std::unordered_map<int, StringViewMap<std::uint64_t>>
IndexDatabase::query_file_category_counts_batch(
    const std::vector<int>& file_ids) const {
    std::unordered_map<int, StringViewMap<std::uint64_t>> results;
    results.reserve(file_ids.size());
    for (const auto file_id : file_ids) {
        std::string value;
        auto status = impl_->db_->get(file_category_counts_key(file_id), &value,
                                      cf::FILE_CAT_COUNTS);
        if (status.IsNotFound()) {
            continue;
        }
        if (!status.ok()) {
            throw_db_error("Failed to read file category counts", status);
        }
        try {
            DecodeContextGuard ctx("file_cat_counts file_id=%d size=%zu",
                                   file_id, value.size());
            results.emplace(file_id, decode_count_map_value(value));
        } catch (const std::exception& e) {
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Corrupt file_cat_counts payload file_id=" +
                                   std::to_string(file_id) +
                                   " size=" + std::to_string(value.size()) +
                                   ": " + e.what());
        }
    }
    return results;
}

std::unordered_map<int, StringViewMap<std::uint64_t>>
IndexDatabase::query_file_pid_tid_counts_batch(
    const std::vector<int>& file_ids) const {
    std::unordered_map<int, StringViewMap<std::uint64_t>> results;
    results.reserve(file_ids.size());
    for (const auto file_id : file_ids) {
        std::string value;
        auto status = impl_->db_->get(file_pid_tid_counts_key(file_id), &value,
                                      cf::FILE_PID_TID_COUNTS);
        if (status.IsNotFound()) {
            continue;
        }
        if (!status.ok()) {
            throw_db_error("Failed to read file pid_tid counts", status);
        }
        try {
            DecodeContextGuard ctx("file_pid_tid_counts file_id=%d size=%zu",
                                   file_id, value.size());
            results.emplace(file_id, decode_count_map_value(value));
        } catch (const std::exception& e) {
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Corrupt file_pid_tid_counts payload file_id=" +
                                   std::to_string(file_id) +
                                   " size=" + std::to_string(value.size()) +
                                   ": " + e.what());
        }
    }
    return results;
}

std::unordered_map<int, NameSummaryResult>
IndexDatabase::query_file_name_summaries_batch(
    const std::vector<int>& file_ids) const {
    std::unordered_map<int, NameSummaryResult> results;
    results.reserve(file_ids.size());
    for (const auto file_id : file_ids) {
        std::string value;
        auto status = impl_->db_->get(file_name_counts_key(file_id), &value,
                                      cf::FILE_NAME_COUNTS);
        if (status.IsNotFound()) {
            continue;
        }
        if (!status.ok()) {
            throw_db_error("Failed to read file name counts", status);
        }
        try {
            DecodeContextGuard ctx("file_name_counts file_id=%d size=%zu",
                                   file_id, value.size());
            results.emplace(file_id, decode_name_summary_value(value));
        } catch (const std::exception& e) {
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Corrupt file_name_counts payload file_id=" +
                                   std::to_string(file_id) +
                                   " size=" + std::to_string(value.size()) +
                                   ": " + e.what());
        }
    }
    return results;
}

std::optional<RootStatisticsResult> IndexDatabase::query_root_scalar_stats()
    const {
    std::string value;
    auto status =
        impl_->db_->get(root_scalar_stats_key(), &value, cf::ROOT_SCALAR_STATS);
    if (status.IsNotFound()) {
        return std::nullopt;
    }
    if (!status.ok()) {
        throw_db_error("Failed to read root scalar statistics", status);
    }
    try {
        DecodeContextGuard ctx("root_scalar_stats size=%zu", value.size());
        return decode_root_scalar_stats_value(value);
    } catch (const std::exception& e) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Corrupt root_scalar_stats payload size=" +
                               std::to_string(value.size()) + ": " + e.what());
    }
}

namespace {

// Read a root count-map CF value, returning an empty map when absent. Throws
// `read_error` on a non-NotFound read failure and a Corrupt-payload error
// (tagged `name`) on a decode failure.
StringViewMap<std::uint64_t> query_root_count_map(
    const rocks::RocksDatabase& db, std::string_view count_key,
    std::string_view column_family, std::string_view read_error,
    const char* name) {
    std::string value;
    auto status = db.get(count_key, &value, column_family);
    if (status.IsNotFound()) {
        return {};
    }
    if (!status.ok()) {
        throw_db_error(read_error, status);
    }
    try {
        DecodeContextGuard ctx("%s size=%zu", name, value.size());
        return decode_count_map_value(value);
    } catch (const std::exception& e) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Corrupt " + std::string(name) + " payload size=" +
                               std::to_string(value.size()) + ": " + e.what());
    }
}

}  // namespace

StringViewMap<std::uint64_t> IndexDatabase::query_root_category_counts() const {
    return query_root_count_map(
        *impl_->db_, root_category_counts_key(), cf::ROOT_CAT_COUNTS,
        "Failed to read root category counts", "root_cat_counts");
}

StringViewMap<std::uint64_t> IndexDatabase::query_root_pid_tid_counts() const {
    return query_root_count_map(
        *impl_->db_, root_pid_tid_counts_key(), cf::ROOT_PID_TID_COUNTS,
        "Failed to read root pid_tid counts", "root_pid_tid_counts");
}

StringViewMap<std::uint64_t> IndexDatabase::query_root_name_counts() const {
    return query_root_count_map(
        *impl_->db_, root_name_counts_key(), cf::ROOT_NAME_COUNTS,
        "Failed to read root name counts", "root_name_counts");
}

namespace {

// Read a root count-map CF value (absent is a no-op) and fold its entries into
// `target`, throwing `error_message` on a non-NotFound read failure.
void merge_root_counts_into(const rocks::RocksDatabase& db,
                            std::string_view count_key,
                            std::string_view column_family,
                            std::string_view error_message,
                            StringViewMap<std::uint64_t>& target) {
    std::string value;
    auto status = db.get(count_key, &value, column_family);
    if (status.IsNotFound()) {
        return;
    }
    if (!status.ok()) {
        throw_db_error(error_message, status);
    }
    for_each_count_map_entry(
        value, [&target](std::string_view key, std::uint64_t count) {
            auto entry = target.try_emplace(std::string(key), 0);
            entry.first->second += count;
        });
}

}  // namespace

void IndexDatabase::merge_root_category_counts_into(
    ChunkStatistics& target) const {
    merge_root_counts_into(
        *impl_->db_, root_category_counts_key(), cf::ROOT_CAT_COUNTS,
        "Failed to read root category counts", target.category_counts);
}

void IndexDatabase::merge_root_pid_tid_counts_into(
    ChunkStatistics& target) const {
    merge_root_counts_into(
        *impl_->db_, root_pid_tid_counts_key(), cf::ROOT_PID_TID_COUNTS,
        "Failed to read root pid_tid counts", target.pid_tid_counts);
}

void IndexDatabase::merge_root_name_counts_into(ChunkStatistics& target) const {
    merge_root_counts_into(
        *impl_->db_, root_name_counts_key(), cf::ROOT_NAME_COUNTS,
        "Failed to read root name counts", target.name_counts);
}

std::vector<int> IndexDatabase::query_name_file_postings(
    std::string_view name) const {
    auto name_id = query_name_id(name);
    if (!name_id) {
        return {};
    }

    std::vector<int> results;
    std::string prefix("n|");
    rocks::KeyCodec::append_be64(prefix, *name_id);
    scan_prefix(
        *impl_->db_, cf::NAME_FILE_POSTINGS, prefix,
        [&results](::rocksdb::Iterator& it) {
            auto key = iterator_key(it);
            // "n|" (2) + be64 name_id (8) + be32 file_id (4) = 14 bytes
            if (key.size() != 14) return;
            results.push_back(static_cast<int>(rocks::KeyCodec::decode_be32(
                std::string_view(key.data() + 10, 4))));
        });
    return results;
}

std::vector<std::uint64_t> IndexDatabase::query_name_chunk_postings(
    std::string_view name, int file_id) const {
    auto name_id = query_name_id(name);
    if (!name_id) {
        return {};
    }

    std::vector<std::uint64_t> results;
    std::string prefix("n|");
    rocks::KeyCodec::append_be64(prefix, *name_id);
    rocks::KeyCodec::append_be32(prefix, static_cast<std::uint32_t>(file_id));
    scan_prefix(*impl_->db_, cf::NAME_CHUNK_POSTINGS, prefix,
                [&results](::rocksdb::Iterator& it) {
                    auto key = iterator_key(it);
                    // "n|" (2) + be64 name_id (8) + be32 file_id (4) +
                    //     be64 checkpoint_idx (8) = 22 bytes
                    if (key.size() != 22) return;
                    results.push_back(rocks::KeyCodec::decode_be64(
                        std::string_view(key.data() + 14, 8)));
                });
    return results;
}

std::vector<GzipMemberRecord> IndexDatabase::query_gzip_members(
    int file_id) const {
    std::vector<GzipMemberRecord> members;
    const auto prefix = encoding::gzip_member_prefix(file_id);
    scan_prefix(
        *impl_->db_, rocks::cf::MEMBERS, prefix, [&](::rocksdb::Iterator& it) {
            members.push_back(
                decode_gzip_member(iterator_key(it), iterator_value(it)));
        });
    std::sort(members.begin(), members.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.member_idx < rhs.member_idx;
              });
    return members;
}

std::vector<ChunkSpan> IndexDatabase::query_chunk_spans(int file_id) const {
    auto members = query_gzip_members(file_id);
    std::vector<ChunkSpan> spans;
    spans.reserve(members.size());
    for (const auto& m : members) {
        spans.push_back(
            {m.uc_offset, m.uc_size, m.first_line_num, m.last_line_num});
    }
    return spans;
}

TimeBounds IndexDatabase::query_time_bounds(int file_id) const {
    TimeBounds bounds;
    for (const auto& row : query_chunk_statistics(file_id)) {
        const auto min_ts = row.stats.min_timestamp_us;
        const auto max_ts = row.stats.max_timestamp_us;
        if (min_ts == std::numeric_limits<std::uint64_t>::max() ||
            max_ts == 0) {
            continue;
        }
        bounds.valid = true;
        bounds.min_timestamp_us = std::min(bounds.min_timestamp_us, min_ts);
        bounds.max_timestamp_us = std::max(bounds.max_timestamp_us, max_ts);
    }
    return bounds;
}

std::vector<ChunkDimensionStatsResult>
IndexDatabase::query_chunk_dimension_stats(int file_id) const {
    std::vector<ChunkDimensionStatsResult> results;
    const auto prefix = prefix_for_file(file_id);
    scan_prefix(*impl_->db_, cf::CHUNK_DIM_STATS, prefix,
                [&](::rocksdb::Iterator& it) {
                    results.push_back(decode_chunk_dimension_stats_value(
                        iterator_key(it), iterator_value(it)));
                });
    std::sort(results.begin(), results.end(),
              [](const auto& lhs, const auto& rhs) {
                  return std::tie(lhs.checkpoint_idx, lhs.dimension) <
                         std::tie(rhs.checkpoint_idx, rhs.dimension);
              });
    return results;
}

std::unordered_map<int, std::vector<ChunkDimensionStatsResult>>
IndexDatabase::query_chunk_dimension_stats_batch(
    const std::vector<int>& file_ids) const {
    std::unordered_map<int, std::vector<ChunkDimensionStatsResult>> results;
    if (file_ids.empty()) {
        return results;
    }
    results.reserve(file_ids.size());

    auto status = for_each_file_in_batch(
        *impl_->db_, cf::CHUNK_DIM_STATS, file_ids,
        [&](int file_id, ::rocksdb::Iterator& it, const std::string& key) {
            results[file_id].push_back(
                decode_chunk_dimension_stats_value(key, iterator_value(it)));
        });
    if (!status.ok()) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to batch query chunk dimension stats: " +
                               status.ToString());
    }

    for (auto& [_, entries] : results) {
        std::sort(entries.begin(), entries.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return std::tie(lhs.checkpoint_idx, lhs.dimension) <
                             std::tie(rhs.checkpoint_idx, rhs.dimension);
                  });
    }
    return results;
}

std::vector<ChunkDimensionStatsResult>
IndexDatabase::query_chunk_dimension_stats_for_dimension(
    int file_id, std::string_view dimension) const {
    std::vector<ChunkDimensionStatsResult> results;
    const auto prefix = prefix_for_file(file_id);
    scan_prefix(*impl_->db_, cf::CHUNK_DIM_STATS, prefix,
                [&](::rocksdb::Iterator& it) {
                    auto decoded = decode_chunk_dimension_stats_value(
                        iterator_key(it), iterator_value(it));
                    if (decoded.dimension == dimension) {
                        results.push_back(std::move(decoded));
                    }
                });
    std::sort(results.begin(), results.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.checkpoint_idx < rhs.checkpoint_idx;
              });
    return results;
}

namespace {
// The distinct PIDs a file touched, projected from its "pid:tid" -> count map
// (FILE_PID_TID_COUNTS, written by the bloom/stats pass). This is the same set
// the retired manifest P| keys used to store, now derived so PID collection
// needs no separate index tier.
std::unordered_set<std::uint64_t> pids_from_pid_tid_counts(
    const StringViewMap<std::uint64_t>& counts) {
    std::unordered_set<std::uint64_t> pids;
    for (const auto& [key, _] : counts) {
        std::string_view sv(key);
        const auto colon = sv.find(':');
        const std::string_view pid_sv =
            colon == std::string_view::npos ? sv : sv.substr(0, colon);
        std::uint64_t pid = 0;
        std::from_chars(pid_sv.data(), pid_sv.data() + pid_sv.size(), pid);
        pids.insert(pid);
    }
    return pids;
}
}  // namespace

std::unordered_set<std::uint64_t> IndexDatabase::query_file_pids(
    int file_id) const {
    std::string value;
    auto status = impl_->db_->get(file_pid_tid_counts_key(file_id), &value,
                                  cf::FILE_PID_TID_COUNTS);
    if (status.IsNotFound()) return {};
    if (!status.ok()) {
        throw_db_error("Failed to read file PIDs", status);
    }
    return pids_from_pid_tid_counts(decode_count_map_value(value));
}

std::unordered_map<int, std::unordered_set<std::uint64_t>>
IndexDatabase::query_all_file_pids() const {
    std::unordered_map<int, std::unordered_set<std::uint64_t>> result;
    scan_prefix(
        *impl_->db_, cf::FILE_PID_TID_COUNTS, /*prefix=*/"",
        [&](::rocksdb::Iterator& it) {
            auto key = iterator_key(it);
            if (key.size() < sizeof(std::uint32_t)) return;
            auto file_id = static_cast<int>(rocks::KeyCodec::decode_be32(
                std::string_view(key).substr(0, sizeof(std::uint32_t))));
            result[file_id] = pids_from_pid_tid_counts(
                decode_count_map_value(std::string_view(iterator_value(it))));
        });
    return result;
}

namespace {

// Read metadata record field `idx` for `file_id`, 0 when the record is absent.
std::uint64_t get_metadata_field(const rocks::RocksDatabase& db, int file_id,
                                 std::size_t idx) {
    std::string value;
    auto status = db.get(metadata_key(file_id), &value, cf::METADATA);
    if (status.IsNotFound()) {
        return 0;
    }
    if (!status.ok()) {
        throw_db_error("Failed to read metadata", status);
    }
    return decode_metadata_record(value)[idx];
}

}  // namespace

std::uint64_t IndexDatabase::get_checkpoint_size(int file_id) const {
    return get_metadata_field(*impl_->db_, file_id, 0);
}

std::uint64_t IndexDatabase::get_num_lines(int file_id) const {
    return get_metadata_field(*impl_->db_, file_id, 1);
}

std::uint64_t IndexDatabase::get_max_bytes(int file_id) const {
    return get_metadata_field(*impl_->db_, file_id, 2);
}

void IndexDatabase::ensure_hash_tables_cached() const {
    if (!impl_->hash_cache_) {
        impl_->hash_cache_ = std::make_unique<Impl::HashCache>();
    }

    {
        std::shared_lock lock(impl_->hash_cache_->mutex);
        if (impl_->hash_cache_->loaded) return;
    }

    std::unique_lock lock(impl_->hash_cache_->mutex);
    if (impl_->hash_cache_->loaded) return;

    scan_prefix(
        *impl_->db_, cf::HASH_TABLES, "", [this](::rocksdb::Iterator& it) {
            auto key = iterator_key(it);
            if (key.empty()) return;
            auto type = static_cast<std::uint8_t>(key[0]);
            auto payload = key.substr(1);
            auto value = iterator_value(it);

            switch (type) {
                case 0:
                    impl_->hash_cache_->file_hash.emplace(payload, value);
                    break;
                case 1:
                    impl_->hash_cache_->host_hash.emplace(payload, value);
                    break;
                case 2:
                    impl_->hash_cache_->string_hash.emplace(payload, value);
                    break;
                case 3:
                    impl_->hash_cache_->proc_hash.emplace(payload, value);
                    break;
                case 4:
                    impl_->hash_cache_->file_name.emplace(payload, value);
                    break;
                case 5:
                    impl_->hash_cache_->host_name.emplace(payload, value);
                    break;
                case 6:
                    impl_->hash_cache_->string_name.emplace(payload, value);
                    break;
                case 7:
                    impl_->hash_cache_->proc_name.emplace(payload, value);
                    break;
                default:
                    break;
            }
        });
    impl_->hash_cache_->loaded = true;
}

std::unordered_map<std::string, std::string> IndexDatabase::query_hash_table(
    HashType type) const {
    ensure_hash_tables_cached();
    std::shared_lock lock(impl_->hash_cache_->mutex);
    switch (type) {
        case HashType::FILE:
            return impl_->hash_cache_->file_hash;
        case HashType::HOST:
            return impl_->hash_cache_->host_hash;
        case HashType::STRING:
            return impl_->hash_cache_->string_hash;
        case HashType::PROC:
            return impl_->hash_cache_->proc_hash;
    }
    return {};
}

std::uint64_t IndexDatabase::count_hash_entries(HashType type) const {
    const auto prefix = encoding::hash_table_forward_key(
        static_cast<std::uint8_t>(type), std::string_view{});
    auto it = impl_->db_->new_iterator(cf::HASH_TABLES);
    std::uint64_t count = 0;
    for (it->Seek(::rocksdb::Slice(prefix.data(), prefix.size())); it->Valid();
         it->Next()) {
        auto key = it->key();
        if (key.size() < prefix.size() ||
            std::memcmp(key.data(), prefix.data(), prefix.size()) != 0)
            break;
        ++count;
    }
    return count;
}

std::optional<std::string> IndexDatabase::lookup_hash(
    HashType type, std::string_view hash) const {
    if (hash.empty()) return std::nullopt;
    std::string value;
    auto status = impl_->db_->get(
        encoding::hash_table_forward_key(static_cast<std::uint8_t>(type), hash),
        &value, cf::HASH_TABLES);
    if (!status.ok()) return std::nullopt;
    return value;
}

std::optional<std::string> IndexDatabase::resolve_hash(
    HashType type, std::string_view hash) const {
    ensure_hash_tables_cached();
    std::shared_lock lock(impl_->hash_cache_->mutex);
    const std::unordered_map<std::string, std::string>* cache = nullptr;
    switch (type) {
        case HashType::FILE:
            cache = &impl_->hash_cache_->file_hash;
            break;
        case HashType::HOST:
            cache = &impl_->hash_cache_->host_hash;
            break;
        case HashType::STRING:
            cache = &impl_->hash_cache_->string_hash;
            break;
        case HashType::PROC:
            cache = &impl_->hash_cache_->proc_hash;
            break;
    }
    if (cache) {
        auto it = cache->find(std::string(hash));
        if (it != cache->end()) return it->second;
    }
    return std::nullopt;
}

std::optional<std::string> IndexDatabase::resolve_name_to_hash(
    HashType type, std::string_view name) const {
    ensure_hash_tables_cached();
    std::shared_lock lock(impl_->hash_cache_->mutex);
    const std::unordered_map<std::string, std::string>* cache = nullptr;
    switch (type) {
        case HashType::FILE:
            cache = &impl_->hash_cache_->file_name;
            break;
        case HashType::HOST:
            cache = &impl_->hash_cache_->host_name;
            break;
        case HashType::STRING:
            cache = &impl_->hash_cache_->string_name;
            break;
        case HashType::PROC:
            cache = &impl_->hash_cache_->proc_name;
            break;
    }
    if (cache) {
        auto it = cache->find(std::string(name));
        if (it != cache->end()) return it->second;
    }
    return std::nullopt;
}

}  // namespace dftracer::utils::utilities::indexer
