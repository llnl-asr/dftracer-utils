#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/rocksdb/key_codec.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_merge_operator.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_tracker.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics_merge_operator.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_sst_writer_context.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/index_encoding.h>
#include <dftracer/utils/utilities/indexer/internal/payload_codec.h>
#include <dftracer/utils/utilities/indexer/internal/scan_prefix.h>
#include <dftracer/utils/utilities/indexer/internal/statistics_codec.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

namespace dftracer::utils::utilities::indexer {

namespace queries = composites::dft::indexing::queries;
namespace rocks = dftracer::utils::rocksdb;
namespace cf = rocks::cf;

using namespace internal;

namespace {

constexpr std::uint32_t SCHEMA_VERSION = 1;

[[noreturn]] void throw_db_error(std::string_view message,
                                 const ::rocksdb::Status& status) {
    throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                       std::string(message) + ": " + status.ToString());
}

std::string file_lookup_key(std::string_view logical_name) {
    return std::string("f|") + std::string(logical_name);
}

std::string file_reverse_key(int file_id) {
    std::string key("r|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    return key;
}

std::string schema_version_key() { return "_schema_version"; }

IndexFileEntryCapability decode_file_capabilities(std::string_view record) {
    if (record.size() < 5) {
        return IndexFileEntryCapability::NONE;
    }
    return static_cast<IndexFileEntryCapability>(
        static_cast<std::uint8_t>(record[4]));
}

int decode_file_id(std::string_view record) {
    if (record.size() < 4) {
        throw std::runtime_error("Corrupt file record");
    }
    return static_cast<int>(rocks::KeyCodec::decode_be32(record.substr(0, 4)));
}

int decode_prefixed_file_id(std::string_view key) {
    if (key.size() < 4) {
        throw std::runtime_error("Corrupt file-prefixed key");
    }
    return static_cast<int>(rocks::KeyCodec::decode_be32(key.substr(0, 4)));
}

std::uint64_t decode_file_hash(std::string_view record) {
    if (record.size() < 28) {
        throw std::runtime_error("Corrupt file record");
    }
    return rocks::KeyCodec::decode_be64(record.substr(20, 8));
}

using encoding::prefix_for_file;

std::string make_dimension_key(int file_id, std::string_view dimension) {
    std::string key("d|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    key.append(dimension);
    return key;
}

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
std::string root_scalar_stats_key() { return "_root"; }
std::string root_category_counts_key() { return "_root"; }
std::string root_name_counts_key() { return "_root"; }
std::string root_pid_tid_counts_key() { return "_root"; }

using encoding::name_lookup_key;
using encoding::name_reverse_key;
std::string tar_archive_key(int file_id) { return prefix_for_file(file_id); }

ChunkBloomResult decode_chunk_bloom(std::string_view key,
                                    std::string_view value,
                                    std::size_t prefix_size) {
    ChunkBloomResult result;
    auto checkpoint_pos = key.find('\0', prefix_size);
    if (checkpoint_pos == std::string_view::npos ||
        checkpoint_pos + 1 + 8 > key.size()) {
        throw std::runtime_error("Corrupt chunk bloom key");
    }
    result.checkpoint_idx =
        rocks::KeyCodec::decode_be64(key.substr(checkpoint_pos + 1, 8));
    if (value.size() < 8) {
        throw std::runtime_error("Corrupt chunk bloom value");
    }
    result.num_entries = rocks::KeyCodec::decode_be64(value.substr(0, 8));
    result.bloom_data.assign(value.begin() + 8, value.end());
    return result;
}

FileBloomResult decode_file_bloom(std::string_view value) {
    if (value.size() < 8) {
        throw std::runtime_error("Corrupt file bloom value");
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

    auto duration_sketch = cursor.blob();
    if (!duration_sketch.empty()) {
        stats.duration_sketch = common::statistics::DDSketch::deserialize(
            duration_sketch.data(), duration_sketch.size());
    }

    auto duration_histogram = cursor.str();
    if (!duration_histogram.empty()) {
        stats.duration_histogram =
            common::statistics::Log2Histogram::from_json(duration_histogram);
    }

    auto name_sketches = cursor.blob();
    if (!name_sketches.empty()) {
        stats.name_duration_sketches =
            ChunkStatistics::deserialize_name_duration_sketches(
                name_sketches.data(), name_sketches.size());
    }

    stats.name_duration_histograms =
        ChunkStatistics::parse_histogram_map_json(cursor.str());
    stats.name_duration_sums =
        ChunkStatistics::parse_double_map_json(cursor.str());
    stats.name_duration_sum_sqs =
        ChunkStatistics::parse_double_map_json(cursor.str());
    stats.name_category = ChunkStatistics::parse_string_map_json(cursor.str());

    auto ts_hist_blob = cursor.blob();
    if (!ts_hist_blob.empty()) {
        stats.timestamp_histogram =
            common::statistics::TimestampHistogram::deserialize(
                ts_hist_blob.data(), ts_hist_blob.size());
    }

    return stats;
}

IndexerCheckpoint decode_checkpoint(std::string_view key,
                                    std::string_view value) {
    if (key.size() < 20) {
        throw std::runtime_error("Corrupt checkpoint key");
    }

    IndexerCheckpoint checkpoint;
    checkpoint.uc_offset = rocks::KeyCodec::decode_be64(key.substr(4, 8));
    checkpoint.checkpoint_idx = rocks::KeyCodec::decode_be64(key.substr(12, 8));

    Cursor cursor(value);
    checkpoint.uc_size = cursor.u64();
    checkpoint.c_offset = cursor.u64();
    checkpoint.c_size = cursor.u64();
    checkpoint.bits = static_cast<int>(cursor.i64());
    checkpoint.dict_compressed = cursor.blob();
    checkpoint.num_lines = cursor.u64();
    checkpoint.first_line_num = cursor.u64();
    checkpoint.last_line_num = cursor.u64();
    return checkpoint;
}

ChunkDimensionStatsResult decode_chunk_dimension_stats_value(
    std::string_view key, std::string_view value) {
    ChunkDimensionStatsResult result;
    if (key.size() < 12) {
        throw std::runtime_error("Corrupt chunk dimension stats key");
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

std::vector<std::uint32_t> decode_line_numbers(Cursor& cursor) {
    auto blob = cursor.blob();
    return queries::unpack_line_numbers(blob.data(), blob.size());
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

TarArchiveMetadata decode_tar_archive_value(std::string_view value) {
    Cursor cursor(value);
    TarArchiveMetadata metadata;
    metadata.archive_name = cursor.str();
    metadata.checkpoint_size = cursor.u64();
    metadata.total_lines = cursor.u64();
    metadata.total_uc_size = cursor.u64();
    metadata.total_files = cursor.u64();
    return metadata;
}

TarFileRecord decode_tar_file(std::string_view key, std::string_view value) {
    if (key.size() < 13) {
        throw std::runtime_error("Corrupt tar file key");
    }

    const auto name_pos = key.find('\0', 12);
    if (name_pos == std::string_view::npos) {
        throw std::runtime_error("Corrupt tar file key");
    }

    Cursor cursor(value);
    TarFileRecord record;
    record.uncompressed_offset = rocks::KeyCodec::decode_be64(key.substr(4, 8));
    record.file_name = std::string(key.substr(name_pos + 1));
    record.file_size = cursor.u64();
    record.file_mtime = cursor.u64();
    record.typeflag = static_cast<char>(cursor.u8());
    record.data_offset = cursor.u64();
    return record;
}

std::array<std::uint64_t, 3> decode_metadata_record(std::string_view value) {
    Cursor cursor(value);
    return {cursor.u64(), cursor.u64(), cursor.u64()};
}

std::string iterator_value(::rocksdb::Iterator& it) {
    const auto slice = it.value();
    return std::string(slice.data(), slice.size());
}

std::string iterator_key(::rocksdb::Iterator& it) {
    const auto slice = it.key();
    return std::string(slice.data(), slice.size());
}

template <typename Fn>
void scan_prefix(const rocks::RocksDatabase& db, std::string_view column_family,
                 std::string_view prefix, Fn&& fn) {
    internal::scan_prefix_iterator(
        "Failed to scan RocksDB prefix", prefix,
        [&] { return db.new_iterator(column_family); }, std::forward<Fn>(fn));
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
    using dftracer::utils::utilities::composites::dft::aggregators::
        AggregationMergeOperator;
    using dftracer::utils::utilities::composites::dft::aggregators::
        SystemMetricsMergeOperator;
    auto agg_merge_op = std::make_shared<AggregationMergeOperator>();
    auto sys_merge_op = std::make_shared<SystemMetricsMergeOperator>();
    return [agg_merge_op, sys_merge_op](const std::string& cf_name,
                                        ::rocksdb::ColumnFamilyOptions& opts) {
        if (cf_name == cf::AGGREGATION) {
            opts.merge_operator = agg_merge_op;
            ::rocksdb::BlockBasedTableOptions bbt;
            bbt.block_size = 32 * 1024;
            bbt.format_version = 5;
            bbt.index_block_restart_interval = 16;
            bbt.whole_key_filtering = false;
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

IndexDatabase::IndexDatabase(const std::string& index_path,
                             rocks::RocksDatabase::OpenMode open_mode)
    : db_path_(internal::normalize_index_root(index_path)),
      open_mode_(open_mode),
      db_(rocks::RocksDBManager::instance().get_or_open(
          db_path_, open_mode_, make_aggregation_cf_override())) {
    if (open_mode_ == rocks::RocksDatabase::OpenMode::ReadWrite) {
        init_schema();
    }
}

std::unique_ptr<IndexDatabaseWriterContext> IndexDatabase::begin_write() {
    return std::unique_ptr<IndexDatabaseWriterContext>(
        new IndexDatabaseWriterContext(db_));
}

void IndexDatabase::bulk_ingest(
    const SstArtifactRegistry& registry,
    const std::unordered_set<std::string>& skip_cfs) {
    const auto skipped = [&](std::string_view cf_name) {
        return skip_cfs.find(std::string(cf_name)) != skip_cfs.end();
    };
    const auto ingest = [&](std::string_view cf_name,
                            const std::vector<std::string>& files) {
        if (skipped(cf_name)) return;
        auto status = db_->ingest_external_files(cf_name, files,
                                                 /*ingest_behind=*/false);
        if (!status.ok()) {
            throw_db_error("Failed to ingest SSTs into column family '" +
                               std::string(cf_name) + "'",
                           status);
        }
    };

    ingest(cf::METADATA, registry.metadata());
    ingest(cf::CHECKPOINTS, registry.checkpoints());
    ingest(cf::MANIFEST, registry.manifest());
    ingest(cf::CHUNK_BLOOM, registry.chunk_bloom());
    ingest(cf::FILE_BLOOM, registry.file_bloom());
    ingest(cf::CHUNK_STATS, registry.chunk_stats());
    ingest(cf::CHUNK_DIM_STATS, registry.chunk_dim_stats());
    ingest(cf::DIMENSIONS, registry.dimensions());
    ingest(cf::FILE_SCALAR_STATS, registry.file_scalar_stats());
    ingest(cf::FILE_CAT_COUNTS, registry.file_cat_counts());
    ingest(cf::FILE_PID_TID_COUNTS, registry.file_pid_tid_counts());
    ingest(cf::FILE_NAME_COUNTS, registry.file_name_counts());
    // Multiple workers emit identical (name_id, name) dictionary pairs for
    // shared event names, so SSTs across workers have overlapping key ranges.
    // Regular ingest forbids overlap *within a single call*, so we ingest one
    // SST at a time. The content-addressed values are deterministic (same
    // name -> same hash), so the normal LSM sequence-number semantics (later
    // ingest shadows earlier with identical value) preserve correctness
    // without requiring `ingest_behind`.
    if (!skipped(cf::NAME_DICTIONARY)) {
        for (const auto& path : registry.name_dictionary()) {
            auto status = db_->ingest_external_files(
                cf::NAME_DICTIONARY, {path}, /*ingest_behind=*/false);
            if (!status.ok()) {
                throw_db_error(
                    "Failed to ingest SST into column family 'name_dictionary'",
                    status);
            }
        }
    }
    ingest(cf::NAME_FILE_POSTINGS, registry.name_file_postings());
    ingest(cf::NAME_CHUNK_POSTINGS, registry.name_chunk_postings());
    // HASH_TABLES is content-addressed: same hash -> same name across workers.
    // Same rationale as NAME_DICTIONARY: ingest one SST at a time so rocksdb
    // can place overlapping files at L0 with new seqnos; deterministic values
    // mean last-writer-wins resolves correctly.
    if (!skipped(cf::HASH_TABLES)) {
        for (const auto& path : registry.hash_tables()) {
            auto status = db_->ingest_external_files(cf::HASH_TABLES, {path},
                                                     /*ingest_behind=*/false);
            if (!status.ok()) {
                throw_db_error(
                    "Failed to ingest SST into column family 'hash_tables'",
                    status);
            }
        }
    }
    // AGGREGATION + SYSTEM_METRICS: workers emit mixed Put+Merge SSTs with
    // overlapping (pid, time_bucket, ...) keys across workers. Ingest one
    // SST at a time; the rocksdb merge_operator on these CFs collapses
    // cross-worker merge operands at read/compaction time.
    if (!skipped(cf::AGGREGATION)) {
        for (const auto& path : registry.aggregation()) {
            auto status = db_->ingest_external_files(cf::AGGREGATION, {path},
                                                     /*ingest_behind=*/false);
            if (!status.ok()) {
                throw_db_error(
                    "Failed to ingest SST into column family 'aggregation'",
                    status);
            }
        }
    }
    if (!skipped(cf::SYSTEM_METRICS)) {
        for (const auto& path : registry.system_metrics()) {
            auto status = db_->ingest_external_files(cf::SYSTEM_METRICS, {path},
                                                     /*ingest_behind=*/false);
            if (!status.ok()) {
                throw_db_error(
                    "Failed to ingest SST into column family 'system_metrics'",
                    status);
            }
        }
    }
}

void IndexDatabase::rebuild_root_summaries() {
    auto writer = begin_write();
    writer->rebuild_root_summaries();
    writer->commit();
}

void IndexDatabase::write_agg_global_config(std::uint64_t time_interval_us,
                                            std::uint32_t config_hash) {
    using dftracer::utils::utilities::composites::dft::aggregators::
        AGG_GLOBAL_CONFIG_KEY;
    using dftracer::utils::utilities::composites::dft::aggregators::
        AggGlobalConfig;
    using dftracer::utils::utilities::composites::dft::aggregators::
        serialize_agg_global_config;

    AggGlobalConfig cfg;
    cfg.time_interval_us = time_interval_us;
    cfg.config_hash = config_hash;
    auto status = db_->put(std::string_view(AGG_GLOBAL_CONFIG_KEY, 2),
                           serialize_agg_global_config(cfg), cf::AGGREGATION);
    if (!status.ok()) {
        throw_db_error("Failed to write aggregation global config", status);
    }
}

void IndexDatabase::write_aggregation_tracker(
    const std::vector<std::string>& blobs) {
    using dftracer::utils::utilities::composites::dft::aggregators::
        AssociationTracker;

    AssociationTracker unified;
    for (const auto& b : blobs) {
        if (b.empty()) continue;
        unified.merge(AssociationTracker::deserialize(b));
    }
    unified.finalize();
    constexpr std::string_view TRACKER_KEY = "__tracker__";
    auto status = db_->put(TRACKER_KEY, unified.serialize(), cf::AGGREGATION);
    if (!status.ok()) {
        throw_db_error("Failed to write aggregation tracker", status);
    }
}

void IndexDatabase::write_agg_file_markers(const std::vector<int>& file_ids) {
    using dftracer::utils::utilities::composites::dft::aggregators::
        make_agg_file_key;

    auto batch = db_->begin_batch();
    for (int file_id : file_ids) {
        if (file_id < 0) continue;
        db_->put(batch, cf::AGGREGATION,
                 make_agg_file_key(static_cast<std::int32_t>(file_id)), "");
    }
    auto status = db_->commit_batch(batch);
    if (!status.ok()) {
        throw_db_error("Failed to write aggregation file markers", status);
    }
}

std::vector<int> IndexDatabase::register_files(
    const std::vector<std::string>& file_paths, bool build_manifest) {
    IndexFileEntryCapability caps = IndexFileEntryCapability::BLOOM |
                                    IndexFileEntryCapability::CHECKPOINTS |
                                    IndexFileEntryCapability::FILE_SUMMARY |
                                    IndexFileEntryCapability::INDEXING_COMPLETE;
    if (build_manifest) {
        caps |= IndexFileEntryCapability::MANIFEST;
    }

    std::vector<int> ids;
    ids.reserve(file_paths.size());
    auto writer = begin_write();
    for (const auto& path : file_paths) {
        const auto logical = internal::get_logical_path(path);
        const auto file_hash = internal::calculate_file_hash(path);
        ids.push_back(
            writer->get_or_create_file_info(logical, file_hash, caps));
    }
    writer->commit();
    return ids;
}

int IndexDatabase::reserve_file_id_range(std::size_t count) {
    if (count == 0) {
        // Return the next id without advancing the counter.
        std::string value;
        const auto key = std::string(encoding::NEXT_FILE_ID_KEY);
        auto status = db_->get(key, &value);
        if (status.IsNotFound()) return 1;
        if (!status.ok()) {
            throw_db_error("Failed to read next file id", status);
        }
        return static_cast<int>(rocks::KeyCodec::decode_be32(value));
    }

    std::string value;
    const auto key = std::string(encoding::NEXT_FILE_ID_KEY);
    auto status = db_->get(key, &value);

    std::uint32_t first = 1;
    if (status.ok()) {
        first = rocks::KeyCodec::decode_be32(value);
    } else if (!status.IsNotFound()) {
        throw_db_error("Failed to read next file id", status);
    }

    const std::uint32_t next = first + static_cast<std::uint32_t>(count);
    const auto encoded = rocks::KeyCodec::encode_be32(next);
    auto put_status = db_->put(key, encoded);
    if (!put_status.ok()) {
        throw_db_error("Failed to advance next file id", put_status);
    }
    return static_cast<int>(first);
}

void IndexDatabase::init_schema() {
    std::string value;
    auto status = db_->get(schema_version_key(), &value);
    if (status.IsNotFound()) {
        status = db_->put(schema_version_key(),
                          rocks::KeyCodec::encode_be32(SCHEMA_VERSION));
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
    scan_prefix(*db_, cf::CHUNK_BLOOM, prefix,
                [&found](::rocksdb::Iterator&) { found = true; });
    return found;
}

bool IndexDatabase::has_manifest_data(int file_id) const {
    auto caps = get_file_capabilities(file_id);
    if (has_capability(caps, IndexFileEntryCapability::MANIFEST)) return true;
    bool found = false;
    std::string prefix("E|");
    rocks::KeyCodec::append_be32(prefix, static_cast<std::uint32_t>(file_id));
    scan_prefix(*db_, cf::MANIFEST, prefix,
                [&found](::rocksdb::Iterator&) { found = true; });
    return found;
}

IndexFileEntryCapability IndexDatabase::get_file_capabilities(
    int file_id) const {
    std::string name;
    auto status = db_->get(file_reverse_key(file_id), &name);
    if (!status.ok()) return IndexFileEntryCapability::NONE;

    std::string record;
    status = db_->get(file_lookup_key(name), &record);
    if (!status.ok()) return IndexFileEntryCapability::NONE;

    return decode_file_capabilities(record);
}

int IndexDatabase::get_file_info_id(std::string_view path) const {
    std::string value;
    auto status = db_->get(file_lookup_key(path), &value);
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
    auto status = db_->get(file_lookup_key(path), &value);
    if (status.IsNotFound()) {
        return std::nullopt;
    }
    if (!status.ok()) {
        throw_db_error("Failed to look up file hash", status);
    }
    return decode_file_hash(value);
}

std::unordered_map<std::string, int> IndexDatabase::query_all_file_info_ids()
    const {
    std::unordered_map<std::string, int> results;
    internal::scan_prefix_iterator(
        "Failed to scan file registry", "f|",
        [this] { return db_->new_iterator(); },
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
        [this] { return db_->new_iterator(); },
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

std::unordered_set<int> IndexDatabase::query_files_with_file_scalar_stats()
    const {
    std::unordered_set<int> results;
    auto it = db_->new_iterator(cf::FILE_SCALAR_STATS);
    for (it->SeekToFirst(); it->Valid();) {
        auto key = iterator_key(*it);
        int file_id = decode_prefixed_file_id(key);
        results.insert(file_id);
        if (file_id == std::numeric_limits<int>::max()) {
            break;
        }
        auto next_prefix = prefix_for_file(file_id + 1);
        it->Seek(::rocksdb::Slice(next_prefix.data(), next_prefix.size()));
    }

    const auto status = it->status();
    if (!status.ok()) {
        throw IndexerError(
            IndexerError::Type::DATABASE_ERROR,
            "Failed to scan file scalar stats: " + status.ToString());
    }

    return results;
}

std::unordered_set<int> IndexDatabase::query_files_with_bloom_data() const {
    std::unordered_set<int> results;
    auto it = db_->new_iterator(cf::CHUNK_BLOOM);
    for (it->SeekToFirst(); it->Valid();) {
        auto key = iterator_key(*it);
        int file_id = decode_prefixed_file_id(key);
        results.insert(file_id);
        if (file_id == std::numeric_limits<int>::max()) {
            break;
        }
        auto next_prefix = prefix_for_file(file_id + 1);
        it->Seek(::rocksdb::Slice(next_prefix.data(), next_prefix.size()));
    }

    const auto status = it->status();
    if (!status.ok()) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to scan bloom data: " + status.ToString());
    }
    return results;
}

int IndexDatabase::find_file(std::string_view file_path) const {
    return get_file_info_id(internal::get_logical_path(file_path));
}

std::optional<std::uint64_t> IndexDatabase::query_name_id(
    std::string_view name) const {
    std::string value;
    auto status = db_->get(name_lookup_key(name), &value, cf::NAME_DICTIONARY);
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
        db_->get(name_reverse_key(name_id), &value, cf::NAME_DICTIONARY);
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
    auto status =
        db_->get(file_scalar_stats_key(file_id), &value, cf::FILE_SCALAR_STATS);
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
    scan_prefix(*db_, cf::CHUNK_BLOOM, prefix, [&](::rocksdb::Iterator& it) {
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
    auto status =
        db_->get(file_bloom_key(file_id, dimension), &value, cf::FILE_BLOOM);
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
    scan_prefix(*db_, cf::DIMENSIONS, prefix, [&](::rocksdb::Iterator& it) {
        auto key = iterator_key(it);
        dimensions.push_back(key.substr(prefix.size()));
    });
    return dimensions;
}

bool IndexDatabase::has_index_dimension(int file_id,
                                        std::string_view dimension) const {
    std::string value;
    return db_
        ->get(make_dimension_key(file_id, dimension), &value, cf::DIMENSIONS)
        .ok();
}

std::vector<ChunkStatisticsResult> IndexDatabase::query_chunk_statistics(
    int file_id) const {
    std::vector<ChunkStatisticsResult> results;
    const auto prefix = prefix_for_file(file_id);
    scan_prefix(*db_, cf::CHUNK_STATS, prefix, [&](::rocksdb::Iterator& it) {
        ChunkStatisticsResult result;
        auto key = iterator_key(it);
        result.checkpoint_idx =
            rocks::KeyCodec::decode_be64(std::string_view(key).substr(4, 8));
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

    std::unordered_set<int> wanted(file_ids.begin(), file_ids.end());
    const auto [min_it, max_it] =
        std::minmax_element(file_ids.begin(), file_ids.end());
    const auto min_prefix = prefix_for_file(*min_it);
    const int max_file_id = *max_it;

    auto it = db_->new_iterator(cf::CHUNK_STATS);
    for (it->Seek(::rocksdb::Slice(min_prefix.data(), min_prefix.size()));
         it->Valid(); it->Next()) {
        auto key = iterator_key(*it);
        int file_id = decode_prefixed_file_id(key);
        if (file_id > max_file_id) {
            break;
        }
        if (!wanted.contains(file_id)) {
            continue;
        }

        ChunkStatisticsResult result;
        result.checkpoint_idx =
            rocks::KeyCodec::decode_be64(std::string_view(key).substr(4, 8));
        result.stats = decode_chunk_statistics_value(iterator_value(*it));
        results[file_id].push_back(std::move(result));
    }

    const auto status = it->status();
    if (!status.ok()) {
        throw IndexerError(
            IndexerError::Type::DATABASE_ERROR,
            "Failed to batch query chunk statistics: " + status.ToString());
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

    auto stats_it = db_->new_iterator(cf::CHUNK_STATS);
    for (stats_it->Seek(::rocksdb::Slice(min_prefix.data(), min_prefix.size()));
         stats_it->Valid(); stats_it->Next()) {
        auto key = iterator_key(*stats_it);
        int file_id = decode_prefixed_file_id(key);
        if (file_id > max_file_id) {
            break;
        }
        if (!wanted.contains(file_id)) {
            continue;
        }

        auto decoded = decode_chunk_statistics_value(iterator_value(*stats_it));
        auto& merged = results[file_id];
        if (merged.num_chunks == 0) {
            merged.stats = std::move(decoded);
        } else {
            merged.stats.merge_from(decoded);
        }
        ++merged.num_chunks;
    }

    auto stats_status = stats_it->status();
    if (!stats_status.ok()) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to batch merge chunk statistics: " +
                               stats_status.ToString());
    }

    auto dims_it = db_->new_iterator(cf::CHUNK_DIM_STATS);
    for (dims_it->Seek(::rocksdb::Slice(min_prefix.data(), min_prefix.size()));
         dims_it->Valid(); dims_it->Next()) {
        auto key = iterator_key(*dims_it);
        int file_id = decode_prefixed_file_id(key);
        if (file_id > max_file_id) {
            break;
        }
        if (!wanted.contains(file_id)) {
            continue;
        }

        auto decoded =
            decode_chunk_dimension_stats_value(key, iterator_value(*dims_it));
        if (!decoded.has_value_counts_payload()) continue;
        decoded.ensure_value_counts_decoded();
        if (!decoded.value_counts) continue;

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
    }

    auto dims_status = dims_it->status();
    if (!dims_status.ok()) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to batch merge chunk dimension stats: " +
                               dims_status.ToString());
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
        auto status = db_->get(file_scalar_stats_key(file_id), &value,
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
            throw std::runtime_error(
                "Corrupt file_scalar_stats payload file_id=" +
                std::to_string(file_id) +
                " size=" + std::to_string(value.size()) + ": " + e.what());
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

    std::unordered_set<int> wanted(file_ids.begin(), file_ids.end());
    const auto [min_it, max_it] =
        std::minmax_element(file_ids.begin(), file_ids.end());
    const auto min_prefix = prefix_for_file(*min_it);
    const int max_file_id = *max_it;

    auto it = db_->new_iterator(cf::METADATA);
    for (it->Seek(::rocksdb::Slice(min_prefix.data(), min_prefix.size()));
         it->Valid(); it->Next()) {
        auto key = iterator_key(*it);
        int file_id = decode_prefixed_file_id(key);
        if (file_id > max_file_id) {
            break;
        }
        if (!wanted.contains(file_id)) {
            continue;
        }

        auto value = iterator_value(*it);
        DecodeContextGuard ctx("metadata file_id=%d size=%zu", file_id,
                               value.size());
        auto decoded = decode_metadata_record(value);
        auto& meta = results[file_id];
        meta.checkpoint_size = decoded[0];
        meta.num_lines = decoded[1];
        meta.max_bytes = decoded[2];
    }

    const auto status = it->status();
    if (!status.ok()) {
        throw IndexerError(
            IndexerError::Type::DATABASE_ERROR,
            "Failed to batch read file metadata: " + status.ToString());
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
        auto status = db_->get(file_category_counts_key(file_id), &value,
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
            throw std::runtime_error(
                "Corrupt file_cat_counts payload file_id=" +
                std::to_string(file_id) +
                " size=" + std::to_string(value.size()) + ": " + e.what());
        }
    }
    return results;
}

void IndexDatabase::merge_file_category_counts_batch_into(
    const std::vector<int>& file_ids,
    std::unordered_map<int, ChunkStatistics*>& targets) const {
    for (const auto file_id : file_ids) {
        auto target_it = targets.find(file_id);
        if (target_it == targets.end() || target_it->second == nullptr) {
            continue;
        }

        std::string value;
        auto status = db_->get(file_category_counts_key(file_id), &value,
                               cf::FILE_CAT_COUNTS);
        if (status.IsNotFound()) {
            continue;
        }
        if (!status.ok()) {
            throw_db_error("Failed to read file category counts", status);
        }

        auto* stats = target_it->second;
        DecodeContextGuard ctx("file_cat_counts merge file_id=%d size=%zu",
                               file_id, value.size());
        for_each_count_map_entry(
            value, [stats](std::string_view key, std::uint64_t count) {
                auto entry =
                    stats->category_counts.try_emplace(std::string(key), 0);
                entry.first->second += count;
            });
    }
}

std::unordered_map<int, StringViewMap<std::uint64_t>>
IndexDatabase::query_file_pid_tid_counts_batch(
    const std::vector<int>& file_ids) const {
    std::unordered_map<int, StringViewMap<std::uint64_t>> results;
    results.reserve(file_ids.size());
    for (const auto file_id : file_ids) {
        std::string value;
        auto status = db_->get(file_pid_tid_counts_key(file_id), &value,
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
            throw std::runtime_error(
                "Corrupt file_pid_tid_counts payload file_id=" +
                std::to_string(file_id) +
                " size=" + std::to_string(value.size()) + ": " + e.what());
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
        auto status = db_->get(file_name_counts_key(file_id), &value,
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
            throw std::runtime_error(
                "Corrupt file_name_counts payload file_id=" +
                std::to_string(file_id) +
                " size=" + std::to_string(value.size()) + ": " + e.what());
        }
    }
    return results;
}

void IndexDatabase::merge_file_pid_tid_counts_batch_into(
    const std::vector<int>& file_ids,
    std::unordered_map<int, ChunkStatistics*>& targets) const {
    for (const auto file_id : file_ids) {
        auto target_it = targets.find(file_id);
        if (target_it == targets.end() || target_it->second == nullptr) {
            continue;
        }

        std::string value;
        auto status = db_->get(file_pid_tid_counts_key(file_id), &value,
                               cf::FILE_PID_TID_COUNTS);
        if (status.IsNotFound()) {
            continue;
        }
        if (!status.ok()) {
            throw_db_error("Failed to read file pid_tid counts", status);
        }

        auto* stats = target_it->second;
        DecodeContextGuard ctx("file_pid_tid_counts merge file_id=%d size=%zu",
                               file_id, value.size());
        for_each_count_map_entry(value, [stats](std::string_view key,
                                                std::uint64_t count) {
            auto entry = stats->pid_tid_counts.try_emplace(std::string(key), 0);
            entry.first->second += count;
        });
    }
}

void IndexDatabase::merge_file_name_counts_batch_into(
    const std::vector<int>& file_ids,
    std::unordered_map<int, ChunkStatistics*>& targets) const {
    for (const auto file_id : file_ids) {
        auto target_it = targets.find(file_id);
        if (target_it == targets.end() || target_it->second == nullptr) {
            continue;
        }

        std::string value;
        auto status = db_->get(file_name_counts_key(file_id), &value,
                               cf::FILE_NAME_COUNTS);
        if (status.IsNotFound()) {
            continue;
        }
        if (!status.ok()) {
            throw_db_error("Failed to read file name counts", status);
        }

        auto* stats = target_it->second;
        DecodeContextGuard ctx("file_name_counts merge file_id=%d size=%zu",
                               file_id, value.size());
        for_each_name_summary_entry(value, [stats](std::string_view key,
                                                   std::uint64_t count) {
            auto entry = stats->name_counts.try_emplace(std::string(key), 0);
            entry.first->second += count;
        });
    }
}

std::optional<RootStatisticsResult> IndexDatabase::query_root_scalar_stats()
    const {
    std::string value;
    auto status =
        db_->get(root_scalar_stats_key(), &value, cf::ROOT_SCALAR_STATS);
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
        throw std::runtime_error("Corrupt root_scalar_stats payload size=" +
                                 std::to_string(value.size()) + ": " +
                                 e.what());
    }
}

StringViewMap<std::uint64_t> IndexDatabase::query_root_category_counts() const {
    std::string value;
    auto status =
        db_->get(root_category_counts_key(), &value, cf::ROOT_CAT_COUNTS);
    if (status.IsNotFound()) {
        return {};
    }
    if (!status.ok()) {
        throw_db_error("Failed to read root category counts", status);
    }
    try {
        DecodeContextGuard ctx("root_cat_counts size=%zu", value.size());
        return decode_count_map_value(value);
    } catch (const std::exception& e) {
        throw std::runtime_error("Corrupt root_cat_counts payload size=" +
                                 std::to_string(value.size()) + ": " +
                                 e.what());
    }
}

StringViewMap<std::uint64_t> IndexDatabase::query_root_pid_tid_counts() const {
    std::string value;
    auto status =
        db_->get(root_pid_tid_counts_key(), &value, cf::ROOT_PID_TID_COUNTS);
    if (status.IsNotFound()) {
        return {};
    }
    if (!status.ok()) {
        throw_db_error("Failed to read root pid_tid counts", status);
    }
    try {
        DecodeContextGuard ctx("root_pid_tid_counts size=%zu", value.size());
        return decode_count_map_value(value);
    } catch (const std::exception& e) {
        throw std::runtime_error("Corrupt root_pid_tid_counts payload size=" +
                                 std::to_string(value.size()) + ": " +
                                 e.what());
    }
}

StringViewMap<std::uint64_t> IndexDatabase::query_root_name_counts() const {
    std::string value;
    auto status =
        db_->get(root_name_counts_key(), &value, cf::ROOT_NAME_COUNTS);
    if (status.IsNotFound()) {
        return {};
    }
    if (!status.ok()) {
        throw_db_error("Failed to read root name counts", status);
    }
    try {
        DecodeContextGuard ctx("root_name_counts size=%zu", value.size());
        return decode_count_map_value(value);
    } catch (const std::exception& e) {
        throw std::runtime_error("Corrupt root_name_counts payload size=" +
                                 std::to_string(value.size()) + ": " +
                                 e.what());
    }
}

void IndexDatabase::merge_root_category_counts_into(
    ChunkStatistics& target) const {
    std::string value;
    auto status =
        db_->get(root_category_counts_key(), &value, cf::ROOT_CAT_COUNTS);
    if (status.IsNotFound()) {
        return;
    }
    if (!status.ok()) {
        throw_db_error("Failed to read root category counts", status);
    }
    for_each_count_map_entry(value, [&target](std::string_view key,
                                              std::uint64_t count) {
        auto entry = target.category_counts.try_emplace(std::string(key), 0);
        entry.first->second += count;
    });
}

void IndexDatabase::merge_root_pid_tid_counts_into(
    ChunkStatistics& target) const {
    std::string value;
    auto status =
        db_->get(root_pid_tid_counts_key(), &value, cf::ROOT_PID_TID_COUNTS);
    if (status.IsNotFound()) {
        return;
    }
    if (!status.ok()) {
        throw_db_error("Failed to read root pid_tid counts", status);
    }
    for_each_count_map_entry(
        value, [&target](std::string_view key, std::uint64_t count) {
            auto entry = target.pid_tid_counts.try_emplace(std::string(key), 0);
            entry.first->second += count;
        });
}

void IndexDatabase::merge_root_name_counts_into(ChunkStatistics& target) const {
    std::string value;
    auto status =
        db_->get(root_name_counts_key(), &value, cf::ROOT_NAME_COUNTS);
    if (status.IsNotFound()) {
        return;
    }
    if (!status.ok()) {
        throw_db_error("Failed to read root name counts", status);
    }
    for_each_count_map_entry(
        value, [&target](std::string_view key, std::uint64_t count) {
            auto entry = target.name_counts.try_emplace(std::string(key), 0);
            entry.first->second += count;
        });
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
        *db_, cf::NAME_FILE_POSTINGS, prefix,
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
    scan_prefix(*db_, cf::NAME_CHUNK_POSTINGS, prefix,
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

bool IndexDatabase::find_checkpoint(int file_id, std::size_t target_offset,
                                    IndexerCheckpoint& checkpoint) const {
    if (target_offset == 0 || file_id < 0) {
        return false;
    }

    bool found = false;
    const auto prefix = prefix_for_file(file_id);
    scan_prefix(*db_, rocks::cf::CHECKPOINTS, prefix,
                [&](::rocksdb::Iterator& it) {
                    auto decoded =
                        decode_checkpoint(iterator_key(it), iterator_value(it));
                    if (decoded.uc_offset <= target_offset &&
                        (!found || decoded.uc_offset >= checkpoint.uc_offset)) {
                        checkpoint = std::move(decoded);
                        found = true;
                    }
                });
    return found;
}

std::vector<IndexerCheckpoint> IndexDatabase::query_checkpoints(
    int file_id) const {
    std::vector<IndexerCheckpoint> checkpoints;
    const auto prefix = prefix_for_file(file_id);
    scan_prefix(
        *db_, rocks::cf::CHECKPOINTS, prefix, [&](::rocksdb::Iterator& it) {
            checkpoints.push_back(
                decode_checkpoint(iterator_key(it), iterator_value(it)));
        });
    std::sort(checkpoints.begin(), checkpoints.end(),
              [](const auto& lhs, const auto& rhs) {
                  return std::tie(lhs.uc_offset, lhs.checkpoint_idx) <
                         std::tie(rhs.uc_offset, rhs.checkpoint_idx);
              });
    return checkpoints;
}

std::optional<TarArchiveMetadata> IndexDatabase::query_tar_archive_metadata(
    int file_id) const {
    std::string value;
    auto status = db_->get(tar_archive_key(file_id), &value, cf::ARCHIVES);
    if (status.IsNotFound()) {
        return std::nullopt;
    }
    if (!status.ok()) {
        throw_db_error("Failed to read tar archive metadata", status);
    }
    return decode_tar_archive_value(value);
}

std::vector<TarFileRecord> IndexDatabase::query_tar_files(int file_id) const {
    std::vector<TarFileRecord> files;
    const auto prefix = prefix_for_file(file_id);
    scan_prefix(*db_, cf::TAR_FILES, prefix, [&](::rocksdb::Iterator& it) {
        files.push_back(decode_tar_file(iterator_key(it), iterator_value(it)));
    });
    std::sort(files.begin(), files.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.uncompressed_offset, lhs.file_name) <
               std::tie(rhs.uncompressed_offset, rhs.file_name);
    });
    return files;
}

bool IndexDatabase::find_tar_file(int file_id, std::string_view file_name,
                                  TarFileRecord& record) const {
    for (auto& entry : query_tar_files(file_id)) {
        if (entry.file_name == file_name) {
            record = std::move(entry);
            return true;
        }
    }
    return false;
}

std::vector<TarFileRecord> IndexDatabase::query_tar_files_in_range(
    int file_id, std::uint64_t start_offset, std::uint64_t end_offset) const {
    std::vector<TarFileRecord> files;
    for (auto& entry : query_tar_files(file_id)) {
        const auto entry_end = entry.uncompressed_offset + entry.file_size;
        if (entry.uncompressed_offset < end_offset &&
            entry_end > start_offset) {
            files.push_back(std::move(entry));
        }
    }
    return files;
}

std::vector<IndexerCheckpoint> IndexDatabase::query_checkpoints_for_line_range(
    int file_id, std::uint64_t start_line, std::uint64_t end_line) const {
    std::vector<IndexerCheckpoint> checkpoints;
    for (auto& checkpoint : query_checkpoints(file_id)) {
        if ((checkpoint.first_line_num <= end_line &&
             checkpoint.last_line_num >= start_line) ||
            (checkpoint.first_line_num <= start_line &&
             checkpoint.last_line_num >= end_line)) {
            checkpoints.push_back(std::move(checkpoint));
        }
    }
    return checkpoints;
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
    scan_prefix(*db_, cf::CHUNK_DIM_STATS, prefix,
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

    std::unordered_set<int> wanted(file_ids.begin(), file_ids.end());
    const auto [min_it, max_it] =
        std::minmax_element(file_ids.begin(), file_ids.end());
    const auto min_prefix = prefix_for_file(*min_it);
    const int max_file_id = *max_it;

    auto it = db_->new_iterator(cf::CHUNK_DIM_STATS);
    for (it->Seek(::rocksdb::Slice(min_prefix.data(), min_prefix.size()));
         it->Valid(); it->Next()) {
        auto key = iterator_key(*it);
        int file_id = decode_prefixed_file_id(key);
        if (file_id > max_file_id) {
            break;
        }
        if (!wanted.contains(file_id)) {
            continue;
        }

        results[file_id].push_back(
            decode_chunk_dimension_stats_value(key, iterator_value(*it)));
    }

    const auto status = it->status();
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
    scan_prefix(*db_, cf::CHUNK_DIM_STATS, prefix,
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

std::vector<EventRangeResult> IndexDatabase::query_event_ranges(
    int file_id) const {
    std::vector<EventRangeResult> results;
    std::string prefix("E|");
    rocks::KeyCodec::append_be32(prefix, static_cast<std::uint32_t>(file_id));
    scan_prefix(*db_, cf::MANIFEST, prefix, [&](::rocksdb::Iterator& it) {
        auto key = iterator_key(it);
        auto payload = std::string_view(key).substr(2 + 4 + 8);
        auto split = payload.find('\0');
        if (split == std::string_view::npos) {
            throw std::runtime_error("Corrupt manifest event key");
        }
        EventRangeResult result;
        result.checkpoint_idx =
            rocks::KeyCodec::decode_be64(std::string_view(key).substr(6, 8));
        result.cat = std::string(payload.substr(0, split));
        result.name = std::string(payload.substr(split + 1));
        auto value = iterator_value(it);
        Cursor cursor(value);
        result.event_count = cursor.u64();
        result.line_numbers = decode_line_numbers(cursor);
        results.push_back(std::move(result));
    });
    std::sort(results.begin(), results.end(),
              [](const auto& lhs, const auto& rhs) {
                  return std::tie(lhs.checkpoint_idx, lhs.cat, lhs.name) <
                         std::tie(rhs.checkpoint_idx, rhs.cat, rhs.name);
              });
    return results;
}

std::vector<EventRangeResult> IndexDatabase::query_event_ranges_for_checkpoint(
    int file_id, std::uint64_t checkpoint_idx) const {
    std::vector<EventRangeResult> results;
    for (auto& range : query_event_ranges(file_id)) {
        if (range.checkpoint_idx == checkpoint_idx) {
            results.push_back(std::move(range));
        }
    }
    return results;
}

std::vector<MetadataLinesResult> IndexDatabase::query_metadata_lines(
    int file_id) const {
    std::vector<MetadataLinesResult> results;
    std::string prefix("M|");
    rocks::KeyCodec::append_be32(prefix, static_cast<std::uint32_t>(file_id));
    scan_prefix(*db_, cf::MANIFEST, prefix, [&](::rocksdb::Iterator& it) {
        auto key = iterator_key(it);
        MetadataLinesResult result;
        result.checkpoint_idx =
            rocks::KeyCodec::decode_be64(std::string_view(key).substr(6, 8));
        result.meta_type = key.substr(14);
        auto value = iterator_value(it);
        Cursor cursor(value);
        result.line_numbers = decode_line_numbers(cursor);
        results.push_back(std::move(result));
    });
    std::sort(results.begin(), results.end(),
              [](const auto& lhs, const auto& rhs) {
                  return std::tie(lhs.checkpoint_idx, lhs.meta_type) <
                         std::tie(rhs.checkpoint_idx, rhs.meta_type);
              });
    return results;
}

std::vector<MetadataLinesResult>
IndexDatabase::query_metadata_lines_for_checkpoint(
    int file_id, std::uint64_t checkpoint_idx) const {
    std::vector<MetadataLinesResult> results;
    for (auto& lines : query_metadata_lines(file_id)) {
        if (lines.checkpoint_idx == checkpoint_idx) {
            results.push_back(std::move(lines));
        }
    }
    return results;
}

std::unordered_set<std::uint64_t> IndexDatabase::query_file_pids(
    int file_id) const {
    std::unordered_set<std::uint64_t> pids;
    std::string key("P|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));

    std::string value;
    auto status = db_->get(key, &value, cf::MANIFEST);
    if (status.IsNotFound()) {
        return pids;
    }
    if (!status.ok()) {
        throw_db_error("Failed to read file PIDs", status);
    }

    // Decode: count (varint) + sorted PIDs (each as varint)
    std::size_t off = 0;
    auto decode_varint = [&value, &off]() -> std::uint64_t {
        std::uint64_t v = 0;
        unsigned shift = 0;
        while (off < value.size()) {
            auto b = static_cast<std::uint8_t>(value[off++]);
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return v;
            shift += 7;
        }
        return v;
    };

    auto count = decode_varint();
    pids.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i) {
        pids.insert(decode_varint());
    }
    return pids;
}

std::unordered_map<int, std::unordered_set<std::uint64_t>>
IndexDatabase::query_all_file_pids() const {
    std::unordered_map<int, std::unordered_set<std::uint64_t>> result;
    std::string prefix("P|");
    scan_prefix(*db_, cf::MANIFEST, prefix, [&](::rocksdb::Iterator& it) {
        auto key = iterator_key(it);
        // Key: "P|" + file_id (4 bytes BE)
        auto file_id = static_cast<int>(
            rocks::KeyCodec::decode_be32(std::string_view(key).substr(2, 4)));

        auto value = iterator_value(it);
        std::size_t off = 0;

        auto decode_varint = [&value, &off]() -> std::uint64_t {
            std::uint64_t v = 0;
            unsigned shift = 0;
            while (off < value.size()) {
                auto b = static_cast<std::uint8_t>(value[off++]);
                v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
                if ((b & 0x80) == 0) return v;
                shift += 7;
            }
            return v;
        };

        auto count = decode_varint();
        std::unordered_set<std::uint64_t> pids;
        pids.reserve(count);
        for (std::uint64_t i = 0; i < count; ++i) {
            pids.insert(decode_varint());
        }
        result[file_id] = std::move(pids);
    });
    return result;
}

std::uint64_t IndexDatabase::get_total_events(int file_id) const {
    std::uint64_t total = 0;
    for (const auto& row : query_chunk_statistics(file_id)) {
        total += row.stats.total_events;
    }
    return total > 0 ? total : get_num_lines(file_id);
}

std::uint64_t IndexDatabase::get_checkpoint_size(int file_id) const {
    std::string value;
    auto status = db_->get(metadata_key(file_id), &value, cf::METADATA);
    if (status.IsNotFound()) {
        return 0;
    }
    if (!status.ok()) {
        throw_db_error("Failed to read metadata", status);
    }
    return decode_metadata_record(value)[0];
}

std::uint64_t IndexDatabase::get_num_lines(int file_id) const {
    std::string value;
    auto status = db_->get(metadata_key(file_id), &value, cf::METADATA);
    if (status.IsNotFound()) {
        return 0;
    }
    if (!status.ok()) {
        throw_db_error("Failed to read metadata", status);
    }
    return decode_metadata_record(value)[1];
}

std::uint64_t IndexDatabase::get_max_bytes(int file_id) const {
    std::string value;
    auto status = db_->get(metadata_key(file_id), &value, cf::METADATA);
    if (status.IsNotFound()) {
        return 0;
    }
    if (!status.ok()) {
        throw_db_error("Failed to read metadata", status);
    }
    return decode_metadata_record(value)[2];
}

void IndexDatabase::ensure_hash_tables_cached() const {
    if (!hash_cache_) {
        hash_cache_ = std::make_unique<HashCache>();
    }

    {
        std::shared_lock lock(hash_cache_->mutex);
        if (hash_cache_->loaded) return;
    }

    std::unique_lock lock(hash_cache_->mutex);
    if (hash_cache_->loaded) return;

    scan_prefix(*db_, cf::HASH_TABLES, "", [this](::rocksdb::Iterator& it) {
        auto key = iterator_key(it);
        if (key.empty()) return;
        auto type = static_cast<std::uint8_t>(key[0]);
        auto payload = key.substr(1);
        auto value = iterator_value(it);

        switch (type) {
            case 0:
                hash_cache_->file_hash.emplace(payload, value);
                break;
            case 1:
                hash_cache_->host_hash.emplace(payload, value);
                break;
            case 2:
                hash_cache_->string_hash.emplace(payload, value);
                break;
            case 3:
                hash_cache_->proc_hash.emplace(payload, value);
                break;
            case 4:
                hash_cache_->file_name.emplace(payload, value);
                break;
            case 5:
                hash_cache_->host_name.emplace(payload, value);
                break;
            case 6:
                hash_cache_->string_name.emplace(payload, value);
                break;
            case 7:
                hash_cache_->proc_name.emplace(payload, value);
                break;
            default:
                break;
        }
    });
    hash_cache_->loaded = true;
}

std::unordered_map<std::string, std::string> IndexDatabase::query_hash_table(
    HashType type) const {
    ensure_hash_tables_cached();
    std::shared_lock lock(hash_cache_->mutex);
    switch (type) {
        case HashType::FILE:
            return hash_cache_->file_hash;
        case HashType::HOST:
            return hash_cache_->host_hash;
        case HashType::STRING:
            return hash_cache_->string_hash;
        case HashType::PROC:
            return hash_cache_->proc_hash;
    }
    return {};
}

std::optional<std::string> IndexDatabase::resolve_hash(
    HashType type, std::string_view hash) const {
    ensure_hash_tables_cached();
    std::shared_lock lock(hash_cache_->mutex);
    const std::unordered_map<std::string, std::string>* cache = nullptr;
    switch (type) {
        case HashType::FILE:
            cache = &hash_cache_->file_hash;
            break;
        case HashType::HOST:
            cache = &hash_cache_->host_hash;
            break;
        case HashType::STRING:
            cache = &hash_cache_->string_hash;
            break;
        case HashType::PROC:
            cache = &hash_cache_->proc_hash;
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
    std::shared_lock lock(hash_cache_->mutex);
    const std::unordered_map<std::string, std::string>* cache = nullptr;
    switch (type) {
        case HashType::FILE:
            cache = &hash_cache_->file_name;
            break;
        case HashType::HOST:
            cache = &hash_cache_->host_name;
            break;
        case HashType::STRING:
            cache = &hash_cache_->string_name;
            break;
        case HashType::PROC:
            cache = &hash_cache_->proc_name;
            break;
    }
    if (cache) {
        auto it = cache->find(std::string(name));
        if (it != cache->end()) return it->second;
    }
    return std::nullopt;
}

std::unordered_map<IndexDatabase::HashType,
                   std::unordered_map<std::string, std::string>>
IndexDatabase::query_all_hash_tables() const {
    ensure_hash_tables_cached();
    std::shared_lock lock(hash_cache_->mutex);
    std::unordered_map<HashType, std::unordered_map<std::string, std::string>>
        result;
    result[HashType::FILE] = hash_cache_->file_hash;
    result[HashType::HOST] = hash_cache_->host_hash;
    result[HashType::STRING] = hash_cache_->string_hash;
    result[HashType::PROC] = hash_cache_->proc_hash;
    return result;
}

}  // namespace dftracer::utils::utilities::indexer
