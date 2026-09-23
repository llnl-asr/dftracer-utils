#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/rocksdb/key_codec.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>
#include <dftracer/utils/utilities/indexer/error.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/batch_scan.h>
#include <dftracer/utils/utilities/indexer/internal/column_type_codec.h>
#include <dftracer/utils/utilities/indexer/internal/count_map_scan.h>
#include <dftracer/utils/utilities/indexer/internal/db_error.h>
#include <dftracer/utils/utilities/indexer/internal/index_batch_writer.h>
#include <dftracer/utils/utilities/indexer/internal/index_encoding.h>
#include <dftracer/utils/utilities/indexer/internal/payload_codec.h>
#include <dftracer/utils/utilities/indexer/internal/registry_codec.h>
#include <dftracer/utils/utilities/indexer/internal/scan_prefix.h>
#include <dftracer/utils/utilities/indexer/internal/statistics_codec.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace dftracer::utils::utilities::indexer {

namespace rocks = dftracer::utils::rocksdb;
namespace cf = rocks::cf;

using namespace internal;

namespace {

using encoding::chunk_bloom_key;
using encoding::chunk_dim_stats_key;
using encoding::chunk_stats_key;
using encoding::encode_bloom_value;
using encoding::encode_chunk_dimension_stats_value;
using encoding::encode_chunk_statistics_value;
using encoding::encode_count_map_value;
using encoding::encode_gzip_member_value;
using encoding::encode_metadata_record;
using encoding::encode_name_summary_value;
using encoding::file_bloom_key;
using encoding::file_category_counts_key;
using encoding::file_name_counts_key;
using encoding::file_pid_tid_counts_key;
using encoding::file_scalar_stats_key;
using encoding::gzip_member_key;
using encoding::gzip_member_prefix;
using encoding::make_column_key;
using encoding::make_dimension_key;
using encoding::name_chunk_owner_key;
using encoding::name_chunk_owner_prefix;
using encoding::name_chunk_posting_key;
using encoding::name_file_owner_key;
using encoding::name_file_owner_prefix;
using encoding::name_file_posting_key;
using encoding::name_lookup_key;
using encoding::name_reverse_key;

namespace hash = dftracer::utils::utilities::hash;
using encoding::metadata_key;
using encoding::prefix_for_file;

std::string next_file_id_key() {
    return std::string(encoding::NEXT_FILE_ID_KEY);
}

std::string encode_file_record(
    int file_id, std::uint64_t file_hash,
    IndexFileEntryCapability caps = IndexFileEntryCapability::NONE,
    std::uint64_t file_mtime = 0, std::uint64_t file_size = 0) {
    std::string value;
    rocks::KeyCodec::append_be32(value, static_cast<std::uint32_t>(file_id));
    value.push_back(static_cast<char>(static_cast<std::uint8_t>(caps)));
    value.append(7, '\0');
    append_u64(value, file_mtime);
    append_u64(value, file_hash);
    append_u64(value, file_size);
    return value;
}

std::unordered_map<std::string, std::uint64_t> decode_count_map_value(
    std::string_view value) {
    Cursor cursor(value);
    std::unordered_map<std::string, std::uint64_t> counts;
    auto num_entries = cursor.u32();
    counts.reserve(num_entries);
    for (std::uint32_t i = 0; i < num_entries; ++i) {
        auto key = cursor.str();
        counts.emplace(std::move(key), cursor.u64());
    }
    return counts;
}

}  // namespace

namespace internal {

std::string encode_file_scalar_stats_value(const ChunkStatistics& stats,
                                           std::uint64_t num_chunks) {
    std::string value;
    append_u64(value, stats.total_events);
    append_u64(value, stats.min_timestamp_us);
    append_u64(value, stats.max_timestamp_us);
    append_i64(value, stats.duration_sum_us);
    append_u64(value, stats.duration_min_us);
    append_u64(value, stats.duration_max_us);
    append_u64(value, stats.duration_count);
    append_double(value, stats.duration_m2);

    auto duration_sketch = stats.duration_sketch.serialize();
    append_blob(value, duration_sketch);

    auto duration_histogram = stats.duration_histogram.to_json();
    append_string(value, duration_histogram);

    append_u64(value, num_chunks);

    auto ts_hist = stats.timestamp_histogram.serialize();
    append_blob(value, ts_hist);

    return value;
}

std::string encode_root_scalar_stats_value(
    const ChunkStatistics& stats, std::uint64_t num_chunks,
    std::uint64_t num_files, std::uint64_t total_lines,
    std::uint64_t total_uncompressed_bytes) {
    auto value = encode_file_scalar_stats_value(stats, num_chunks);
    append_u64(value, num_files);
    append_u64(value, total_lines);
    append_u64(value, total_uncompressed_bytes);
    return value;
}

MergedStatisticsResult decode_file_scalar_stats_value(std::string_view value) {
    if (value.size() < 8) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Corrupt file scalar statistics value");
    }
    Cursor cursor(value);
    MergedStatisticsResult result;
    auto& stats = result.stats;
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

    result.num_chunks = cursor.u64();

    auto ts_hist_blob = cursor.blob_view();
    if (!ts_hist_blob.empty()) {
        stats.timestamp_histogram =
            common::statistics::TimestampHistogram::deserialize(
                reinterpret_cast<const std::uint8_t*>(ts_hist_blob.data()),
                ts_hist_blob.size());
    }

    return result;
}

RootStatisticsResult decode_root_scalar_stats_value(std::string_view value) {
    Cursor cursor(value);
    RootStatisticsResult result;
    auto& stats = result.stats;
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

    result.num_chunks = cursor.u64();

    auto ts_hist_blob = cursor.blob_view();
    if (!ts_hist_blob.empty()) {
        stats.timestamp_histogram =
            common::statistics::TimestampHistogram::deserialize(
                reinterpret_cast<const std::uint8_t*>(ts_hist_blob.data()),
                ts_hist_blob.size());
    }

    result.num_files = cursor.u64();
    result.total_lines = cursor.u64();
    result.total_uncompressed_bytes = cursor.u64();
    return result;
}

}  // namespace internal

IndexDatabaseWriterContext::IndexDatabaseWriterContext(
    std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> db)
    : db_(std::move(db)), batch_(db_->begin_batch()) {}

IndexDatabaseWriterContext::IndexDatabaseWriterContext(
    IndexDatabaseWriterContext&&) noexcept = default;

IndexDatabaseWriterContext& IndexDatabaseWriterContext::operator=(
    IndexDatabaseWriterContext&&) noexcept = default;

IndexDatabaseWriterContext::~IndexDatabaseWriterContext() = default;

void IndexDatabaseWriterContext::commit() {
    if (committed_) return;
    auto status = db_->commit_batch(batch_);
    committed_ = true;
    if (!status.ok()) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to commit WriteBatch: " + status.ToString());
    }
}

bool IndexDatabaseWriterContext::has_file_scalar_stats(int file_id) const {
    std::string value;
    auto status =
        db_->get(file_scalar_stats_key(file_id), &value, cf::FILE_SCALAR_STATS);
    return status.ok();
}

void IndexDatabaseWriterContext::init_schema() {
    std::string value;
    auto status = db_->get(schema_version_key(), &value);
    if (status.IsNotFound()) {
        status = db_->put(
            batch_, cf::DEFAULT, schema_version_key(),
            rocks::KeyCodec::encode_be32(IndexDatabase::SCHEMA_VERSION));
        if (!status.ok()) {
            throw_db_error("Failed to initialize schema version", status);
        }
    } else if (!status.ok()) {
        throw_db_error("Failed to read schema version", status);
    }
}

void IndexDatabaseWriterContext::set_file_capabilities(
    int file_id, IndexFileEntryCapability caps) {
    std::string name;
    auto status = db_->get(file_reverse_key(file_id), &name);
    if (!status.ok()) return;
    set_file_capabilities_by_path(name, caps);
}

void IndexDatabaseWriterContext::set_file_capabilities_by_path(
    std::string_view logical_path, IndexFileEntryCapability caps) {
    auto key = file_lookup_key(logical_path);
    std::string record;
    auto status = db_->get(key, &record);
    if (!status.ok() || record.size() < 5) return;

    record[4] = static_cast<char>(static_cast<std::uint8_t>(caps));
    db_->put(batch_, cf::DEFAULT, key, record);
}

void IndexDatabaseWriterContext::add_file_capability(
    int file_id, IndexFileEntryCapability cap) {
    // Inline get_file_capabilities logic
    IndexFileEntryCapability existing = IndexFileEntryCapability::NONE;
    std::string name;
    auto status = db_->get(file_reverse_key(file_id), &name);
    if (status.ok()) {
        std::string record;
        status = db_->get(file_lookup_key(name), &record);
        if (status.ok()) {
            existing = decode_file_capabilities(record);
        }
    }
    set_file_capabilities(file_id, existing | cap);
}

int IndexDatabaseWriterContext::get_or_create_file_info(
    std::string_view path, std::uint64_t file_hash,
    IndexFileEntryCapability caps, std::uint64_t file_mtime,
    std::uint64_t file_size) {
    const auto logical_name = std::string(path);
    const auto lookup = file_lookup_key(logical_name);
    std::string existing;
    auto status = db_->get(lookup, &existing);
    if (status.ok()) {
        const auto file_id = decode_file_id(existing);
        if (decode_file_hash(existing) == file_hash) {
            // Content unchanged: keep the record but refresh the stored
            // mtime/size so a subsequent stat-only staleness check does not
            // report a false positive after a metadata-only touch.
            const auto merged_caps = caps == IndexFileEntryCapability::NONE
                                         ? decode_file_capabilities(existing)
                                         : caps;
            auto refreshed = encode_file_record(file_id, file_hash, merged_caps,
                                                file_mtime, file_size);
            if (refreshed != existing) {
                db_->put(batch_, cf::DEFAULT, lookup, refreshed);
            }
            return file_id;
        }
        delete_file_contents(file_id);
        // Also delete the registry entries for this file
        {
            const auto logical_name_key = file_reverse_key(file_id);
            std::string old_name;
            auto rev_status = db_->get(logical_name_key, &old_name);
            if (rev_status.ok()) {
                db_->del(batch_, cf::DEFAULT, file_lookup_key(old_name));
                db_->del(batch_, cf::DEFAULT, logical_name_key);
            }
            // Delete root summaries
            auto delete_prefix_fn = [&](std::string_view cf_name,
                                        std::string_view prefix) {
                std::vector<std::string> keys;
                scan_prefix(*db_, cf_name, prefix,
                            [&](::rocksdb::Iterator& it) {
                                keys.push_back(iterator_key(it));
                            });
                for (const auto& k : keys) {
                    db_->del(batch_, cf_name, k);
                }
            };
            delete_prefix_fn(cf::ROOT_SCALAR_STATS, root_scalar_stats_key());
            delete_prefix_fn(cf::ROOT_CAT_COUNTS, root_category_counts_key());
            delete_prefix_fn(cf::ROOT_NAME_COUNTS, root_name_counts_key());
            delete_prefix_fn(cf::ROOT_PID_TID_COUNTS,
                             root_pid_tid_counts_key());
        }
        auto registry =
            encode_file_record(file_id, file_hash, caps, file_mtime, file_size);
        status = db_->put(batch_, cf::DEFAULT, lookup, registry);
        if (!status.ok()) {
            throw_db_error("Failed to update file registry", status);
        }
        status = db_->put(batch_, cf::DEFAULT, file_reverse_key(file_id),
                          logical_name);
        if (!status.ok()) {
            throw_db_error("Failed to update reverse file registry", status);
        }
        return file_id;
    }
    if (!status.IsNotFound()) {
        throw_db_error("Failed to query file registry", status);
    }

    std::uint32_t next_id;
    if (cached_next_file_id_ >= 0) {
        next_id = static_cast<std::uint32_t>(cached_next_file_id_);
    } else {
        next_id = 1;
        std::string next_value;
        status = db_->get(next_file_id_key(), &next_value);
        if (status.ok()) {
            next_id = rocks::KeyCodec::decode_be32(next_value);
        } else if (!status.IsNotFound()) {
            throw_db_error("Failed to read next file id", status);
        }
    }
    cached_next_file_id_ = static_cast<std::int64_t>(next_id + 1);

    const auto file_id = static_cast<int>(next_id);
    const auto new_registry =
        encode_file_record(file_id, file_hash, caps, file_mtime, file_size);
    const auto next_registry = rocks::KeyCodec::encode_be32(next_id + 1);

    status = db_->put(batch_, cf::DEFAULT, lookup, new_registry);
    if (!status.ok()) {
        throw_db_error("Failed to insert file registry", status);
    }
    status =
        db_->put(batch_, cf::DEFAULT, file_reverse_key(file_id), logical_name);
    if (!status.ok()) {
        throw_db_error("Failed to insert reverse file registry", status);
    }
    status = db_->put(batch_, cf::DEFAULT, next_file_id_key(), next_registry);
    if (!status.ok()) {
        throw_db_error("Failed to update next file id", status);
    }

    return file_id;
}

void IndexDatabaseWriterContext::put_encoded(std::string_view column_family,
                                             std::string_view key,
                                             std::string_view value,
                                             std::string_view error_message) {
    auto status = db_->put(batch_, column_family, key, value);
    if (!status.ok()) throw_db_error(error_message, status);
}

void IndexDatabaseWriterContext::insert_file_metadata(
    int file_id, std::uint64_t checkpoint_size, std::uint64_t total_lines,
    std::uint64_t total_uc_size) {
    const auto key = metadata_key(file_id);
    const auto value =
        encode_metadata_record(checkpoint_size, total_lines, total_uc_size);
    put_encoded(cf::METADATA, key, value, "Failed to insert metadata");
}

void IndexDatabaseWriterContext::insert_chunk_bloom_filter(
    int file_id, std::uint64_t checkpoint_idx, std::string_view dimension,
    std::span<const unsigned char> blob_data, std::uint64_t num_entries) {
    const auto key = chunk_bloom_key(file_id, dimension, checkpoint_idx);
    const auto value = encode_bloom_value(blob_data, num_entries);
    put_encoded(cf::CHUNK_BLOOM, key, value,
                "Failed to insert chunk bloom filter");
}

void IndexDatabaseWriterContext::insert_chunk_bloom_filter(
    int file_id, std::uint64_t checkpoint_idx, std::string_view dimension,
    const void* blob_data, int blob_size, std::uint64_t num_entries) {
    auto* bytes = static_cast<const unsigned char*>(blob_data);
    insert_chunk_bloom_filter(file_id, checkpoint_idx, dimension,
                              std::span<const unsigned char>(
                                  bytes, static_cast<std::size_t>(blob_size)),
                              num_entries);
}

void IndexDatabaseWriterContext::insert_file_bloom_filter(
    int file_id, std::string_view dimension,
    std::span<const unsigned char> blob_data, std::uint64_t num_entries) {
    const auto key = file_bloom_key(file_id, dimension);
    const auto value = encode_bloom_value(blob_data, num_entries);
    put_encoded(cf::FILE_BLOOM, key, value,
                "Failed to insert file bloom filter");
}

void IndexDatabaseWriterContext::insert_file_bloom_filter(
    int file_id, std::string_view dimension, const void* blob_data,
    int blob_size, std::uint64_t num_entries) {
    auto* bytes = static_cast<const unsigned char*>(blob_data);
    insert_file_bloom_filter(file_id, dimension,
                             std::span<const unsigned char>(
                                 bytes, static_cast<std::size_t>(blob_size)),
                             num_entries);
}

void IndexDatabaseWriterContext::insert_chunk_statistics(
    int file_id, std::uint64_t checkpoint_idx, const ChunkStatistics& stats) {
    const auto key = chunk_stats_key(file_id, checkpoint_idx);
    const auto value = encode_chunk_statistics_value(stats);
    put_encoded(cf::CHUNK_STATS, key, value,
                "Failed to insert chunk statistics");
}

void IndexDatabaseWriterContext::insert_file_scalar_stats(
    int file_id, const ChunkStatistics& stats, std::uint64_t num_chunks) {
    const auto key = file_scalar_stats_key(file_id);
    const auto value = encode_file_scalar_stats_value(stats, num_chunks);
    put_encoded(cf::FILE_SCALAR_STATS, key, value,
                "Failed to insert file scalar statistics");
}

void IndexDatabaseWriterContext::insert_file_category_counts(
    int file_id, const StringViewMap<std::uint64_t>& counts) {
    const auto key = file_category_counts_key(file_id);
    const auto value = encode_count_map_value(counts);
    put_encoded(cf::FILE_CAT_COUNTS, key, value,
                "Failed to insert file category counts");
}

void IndexDatabaseWriterContext::insert_file_pid_tid_counts(
    int file_id, const StringViewMap<std::uint64_t>& counts) {
    const auto key = file_pid_tid_counts_key(file_id);
    const auto value = encode_count_map_value(counts);
    put_encoded(cf::FILE_PID_TID_COUNTS, key, value,
                "Failed to insert file pid_tid counts");
}

void IndexDatabaseWriterContext::insert_file_name_counts(
    int file_id, const StringViewMap<std::uint64_t>& counts) {
    const auto key = file_name_counts_key(file_id);
    const auto value = encode_name_summary_value(counts, 0, counts.size());
    put_encoded(cf::FILE_NAME_COUNTS, key, value,
                "Failed to insert file name counts");
}

void IndexDatabaseWriterContext::insert_name_dictionary_entry(
    std::uint64_t name_id, std::string_view name) {
    const auto encoded_id = rocks::KeyCodec::encode_be64(name_id);
    put_encoded(cf::NAME_DICTIONARY, name_lookup_key(name), encoded_id,
                "Failed to insert name dictionary lookup");
    put_encoded(cf::NAME_DICTIONARY, name_reverse_key(name_id),
                std::string(name), "Failed to insert name dictionary reverse");
}

void IndexDatabaseWriterContext::insert_name_file_posting(std::uint64_t name_id,
                                                          int file_id) {
    const auto key = name_file_posting_key(name_id, file_id);
    const auto owner_key = name_file_owner_key(file_id, name_id);
    put_encoded(cf::NAME_FILE_POSTINGS, key, "",
                "Failed to insert name file posting");
    put_encoded(cf::NAME_FILE_POSTINGS, owner_key, "",
                "Failed to insert name file owner posting");
}

void IndexDatabaseWriterContext::insert_name_chunk_posting(
    std::uint64_t name_id, int file_id, std::uint64_t checkpoint_idx) {
    const auto key = name_chunk_posting_key(name_id, file_id, checkpoint_idx);
    const auto owner_key =
        name_chunk_owner_key(file_id, name_id, checkpoint_idx);
    put_encoded(cf::NAME_CHUNK_POSTINGS, key, "",
                "Failed to insert name chunk posting");
    put_encoded(cf::NAME_CHUNK_POSTINGS, owner_key, "",
                "Failed to insert name chunk owner posting");
}

void IndexDatabaseWriterContext::refresh_root_summaries_after_file_write(
    [[maybe_unused]] int file_id, const ChunkStatistics& stats,
    std::uint64_t num_chunks, bool had_existing_file_summary,
    std::uint64_t file_lines, std::uint64_t file_uncompressed_bytes) {
    auto put_root_scalar = [&](const RootStatisticsResult& root) {
        auto value = encode_root_scalar_stats_value(
            root.stats, root.num_chunks, root.num_files, root.total_lines,
            root.total_uncompressed_bytes);
        put_encoded(cf::ROOT_SCALAR_STATS, root_scalar_stats_key(), value,
                    "Failed to write root scalar statistics");
    };

    auto put_root_counts = [&](std::string_view cf_name, std::string_view key,
                               const auto& counts,
                               std::string_view error_message) {
        auto value = encode_count_map_value(counts);
        put_encoded(cf_name, key, value, error_message);
    };

    if (had_existing_file_summary) {
        rebuild_root_summaries();
        return;
    }

    // Inline query_root_scalar_stats
    std::optional<RootStatisticsResult> root_scalar;
    {
        std::string value;
        auto status =
            db_->get(root_scalar_stats_key(), &value, cf::ROOT_SCALAR_STATS);
        if (status.IsNotFound()) {
            rebuild_root_summaries();
            return;
        }
        if (!status.ok()) {
            throw_db_error("Failed to read root scalar statistics", status);
        }
        try {
            DecodeContextGuard ctx("root_scalar_stats size=%zu", value.size());
            root_scalar = decode_root_scalar_stats_value(value);
        } catch (const std::exception& e) {
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Corrupt root_scalar_stats payload size=" +
                                   std::to_string(value.size()) + ": " +
                                   e.what());
        }
    }

    root_scalar->stats.merge_from(stats);
    root_scalar->num_chunks += num_chunks;
    root_scalar->num_files += 1;
    root_scalar->total_lines += file_lines;
    root_scalar->total_uncompressed_bytes += file_uncompressed_bytes;
    put_root_scalar(*root_scalar);

    // Inline query_root_category_counts
    std::unordered_map<std::string, std::uint64_t> category_counts;
    {
        std::string value;
        auto status =
            db_->get(root_category_counts_key(), &value, cf::ROOT_CAT_COUNTS);
        if (status.ok()) {
            try {
                DecodeContextGuard ctx("root_cat_counts size=%zu",
                                       value.size());
                category_counts = decode_count_map_value(value);
            } catch (const std::exception& e) {
                throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                                   "Corrupt root_cat_counts payload size=" +
                                       std::to_string(value.size()) + ": " +
                                       e.what());
            }
        } else if (!status.IsNotFound()) {
            throw_db_error("Failed to read root category counts", status);
        }
    }
    for (const auto& [key, count] : stats.category_counts) {
        category_counts[key] += count;
    }
    put_root_counts(cf::ROOT_CAT_COUNTS, root_category_counts_key(),
                    category_counts, "Failed to write root category counts");

    // Inline query_root_name_counts
    std::unordered_map<std::string, std::uint64_t> name_counts;
    {
        std::string value;
        auto status =
            db_->get(root_name_counts_key(), &value, cf::ROOT_NAME_COUNTS);
        if (status.ok()) {
            try {
                DecodeContextGuard ctx("root_name_counts size=%zu",
                                       value.size());
                name_counts = decode_count_map_value(value);
            } catch (const std::exception& e) {
                throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                                   "Corrupt root_name_counts payload size=" +
                                       std::to_string(value.size()) + ": " +
                                       e.what());
            }
        } else if (!status.IsNotFound()) {
            throw_db_error("Failed to read root name counts", status);
        }
    }
    for (const auto& [key, count] : stats.name_counts) {
        name_counts[key] += count;
    }
    put_root_counts(cf::ROOT_NAME_COUNTS, root_name_counts_key(), name_counts,
                    "Failed to write root name counts");

    // Inline query_root_pid_tid_counts
    std::unordered_map<std::string, std::uint64_t> pid_tid_counts;
    {
        std::string value;
        auto status = db_->get(root_pid_tid_counts_key(), &value,
                               cf::ROOT_PID_TID_COUNTS);
        if (status.ok()) {
            try {
                DecodeContextGuard ctx("root_pid_tid_counts size=%zu",
                                       value.size());
                pid_tid_counts = decode_count_map_value(value);
            } catch (const std::exception& e) {
                throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                                   "Corrupt root_pid_tid_counts payload size=" +
                                       std::to_string(value.size()) + ": " +
                                       e.what());
            }
        } else if (!status.IsNotFound()) {
            throw_db_error("Failed to read root pid_tid counts", status);
        }
    }
    for (const auto& [key, count] : stats.pid_tid_counts) {
        pid_tid_counts[key] += count;
    }
    put_root_counts(cf::ROOT_PID_TID_COUNTS, root_pid_tid_counts_key(),
                    pid_tid_counts, "Failed to write root pid_tid counts");
}

void IndexDatabaseWriterContext::rebuild_root_summaries() {
    auto put_root_scalar = [&](const RootStatisticsResult& root) {
        auto value = encode_root_scalar_stats_value(
            root.stats, root.num_chunks, root.num_files, root.total_lines,
            root.total_uncompressed_bytes);
        put_encoded(cf::ROOT_SCALAR_STATS, root_scalar_stats_key(), value,
                    "Failed to write root scalar statistics");
    };

    auto put_root_counts = [&](std::string_view cf_name, std::string_view key,
                               const auto& counts,
                               std::string_view error_message) {
        auto value = encode_count_map_value(counts);
        put_encoded(cf_name, key, value, error_message);
    };

    RootStatisticsResult rebuilt;

    // Inline query_all_file_info_ids
    std::unordered_map<std::string, int> all_files;
    internal::scan_prefix_iterator(
        "Failed to scan file registry", "f|",
        [this] { return db_->new_iterator(); },
        [&](::rocksdb::Iterator& it) {
            auto key = iterator_key(it);
            auto value = iterator_value(it);
            all_files.emplace(key.substr(2), decode_file_id(value));
        });

    std::vector<int> file_ids;
    file_ids.reserve(all_files.size());
    for (const auto& [_, existing_file_id] : all_files) {
        file_ids.push_back(existing_file_id);
    }

    rebuilt.num_files = static_cast<std::uint64_t>(file_ids.size());

    {
        auto status = for_each_file_in_batch(
            *db_, cf::FILE_SCALAR_STATS, file_ids,
            [&](int fid, ::rocksdb::Iterator& it, const std::string&) {
                auto value = iterator_value(it);
                try {
                    DecodeContextGuard ctx(
                        "file_scalar_stats file_id=%d size=%zu", fid,
                        value.size());
                    auto row = decode_file_scalar_stats_value(value);
                    rebuilt.stats.merge_from(row.stats);
                    rebuilt.num_chunks += row.num_chunks;
                } catch (const std::exception& e) {
                    throw IndexerError(
                        IndexerError::Type::DATABASE_ERROR,
                        "Corrupt file_scalar_stats payload file_id=" +
                            std::to_string(fid) + " size=" +
                            std::to_string(value.size()) + ": " + e.what());
                }
            });
        if (!status.ok()) {
            throw_db_error("Failed to scan file scalar statistics", status);
        }
    }

    // Inline query_file_metadata_batch
    {
        auto status = for_each_file_in_batch(
            *db_, cf::METADATA, file_ids,
            [&](int fid, ::rocksdb::Iterator& it, const std::string&) {
                auto value = iterator_value(it);
                DecodeContextGuard ctx("metadata file_id=%d size=%zu", fid,
                                       value.size());
                auto decoded = decode_metadata_record(value);
                rebuilt.total_lines += decoded[1];
                rebuilt.total_uncompressed_bytes += decoded[2];
            });
        if (!status.ok()) {
            throw IndexerError(
                IndexerError::Type::DATABASE_ERROR,
                "Failed to batch read file metadata: " + status.ToString());
        }
    }

    if (!file_ids.empty()) {
        std::unordered_set<int> wanted(file_ids.begin(), file_ids.end());
        const auto [min_it, max_it] =
            std::minmax_element(file_ids.begin(), file_ids.end());
        const auto min_prefix = prefix_for_file(*min_it);
        const int max_file_id = *max_it;

        auto scan_counts = [&](std::string_view cf_name,
                               std::string_view error_message, auto& target_map,
                               auto for_each_entry_fn) {
            auto status = for_each_file_in_range(
                *db_, cf_name, min_prefix, max_file_id, wanted,
                [&](int fid, ::rocksdb::Iterator& it, const std::string&) {
                    auto value = iterator_value(it);
                    DecodeContextGuard ctx("%.*s merge file_id=%d size=%zu",
                                           static_cast<int>(cf_name.size()),
                                           cf_name.data(), fid, value.size());
                    for_each_entry_fn(
                        value,
                        [&target_map](std::string_view k, std::uint64_t count) {
                            auto entry =
                                target_map.try_emplace(std::string(k), 0);
                            entry.first->second += count;
                        });
                });
            if (!status.ok()) {
                throw_db_error(std::string(error_message), status);
            }
        };

        scan_counts(cf::FILE_CAT_COUNTS, "Failed to scan file category counts",
                    rebuilt.stats.category_counts,
                    [](std::string_view v, auto cb) {
                        for_each_count_map_entry(v, cb);
                    });
        scan_counts(
            cf::FILE_PID_TID_COUNTS, "Failed to scan file pid_tid counts",
            rebuilt.stats.pid_tid_counts, [](std::string_view v, auto cb) {
                for_each_count_map_entry(v, cb);
            });
        scan_counts(cf::FILE_NAME_COUNTS, "Failed to scan file name counts",
                    rebuilt.stats.name_counts, [](std::string_view v, auto cb) {
                        for_each_name_summary_entry(v, cb);
                    });
    }

    put_root_scalar(rebuilt);
    put_root_counts(cf::ROOT_CAT_COUNTS, root_category_counts_key(),
                    rebuilt.stats.category_counts,
                    "Failed to write root category counts");
    put_root_counts(cf::ROOT_NAME_COUNTS, root_name_counts_key(),
                    rebuilt.stats.name_counts,
                    "Failed to write root name counts");
    put_root_counts(cf::ROOT_PID_TID_COUNTS, root_pid_tid_counts_key(),
                    rebuilt.stats.pid_tid_counts,
                    "Failed to write root pid_tid counts");
}

void IndexDatabaseWriterContext::insert_gzip_member(
    int file_id, const GzipMemberRecord& member) {
    const auto key = gzip_member_key(file_id, member.member_idx);
    const auto value = encode_gzip_member_value(member);
    put_encoded(rocks::cf::MEMBERS, key, value, "Failed to insert gzip member");
}

void IndexDatabaseWriterContext::insert_column(int file_id,
                                               std::string_view column,
                                               ColumnType type) {
    const auto key = make_column_key(file_id, column);
    const auto value = encode_data_type(column_type_to_data_type(type));
    put_encoded(cf::DIMENSIONS, key, value, "Failed to insert column");
}

void IndexDatabaseWriterContext::insert_index_dimension(
    int file_id, std::string_view dimension) {
    const auto key = make_dimension_key(file_id, dimension);
    put_encoded(cf::DIMENSIONS, key, "", "Failed to insert index dimension");
}

void IndexDatabaseWriterContext::insert_hash_table_entry(
    std::uint8_t type, std::string_view hash, std::string_view name) {
    db_->put(batch_, cf::HASH_TABLES,
             encoding::hash_table_forward_key(type, hash), name);
    db_->put(batch_, cf::HASH_TABLES,
             encoding::hash_table_reverse_key(type, name), hash);
}

void IndexDatabaseWriterContext::insert_aggregation_merge(
    std::string_view key, std::string_view operand) {
    auto status = db_->merge(batch_, cf::AGGREGATION, key, operand);
    if (!status.ok()) {
        throw_db_error("Failed to merge aggregation operand", status);
    }
}

void IndexDatabaseWriterContext::insert_aggregation_put(
    std::string_view key, std::string_view value) {
    put_encoded(cf::AGGREGATION, key, value, "Failed to put aggregation value");
}

void IndexDatabaseWriterContext::insert_system_metrics_merge(
    std::string_view key, std::string_view operand) {
    auto status = db_->merge(batch_, cf::SYSTEM_METRICS, key, operand);
    if (!status.ok()) {
        throw_db_error("Failed to merge system metrics operand", status);
    }
}

void IndexDatabaseWriterContext::insert_chunk_dimension_stats(
    int file_id, std::uint64_t checkpoint_idx, const ChunkDimensionStats& stats,
    std::size_t value_counts_cap) {
    const auto key =
        chunk_dim_stats_key(file_id, checkpoint_idx, stats.dimension);
    const auto value =
        encode_chunk_dimension_stats_value(stats, value_counts_cap);
    put_encoded(cf::CHUNK_DIM_STATS, key, value,
                "Failed to insert chunk dimension stats");
}

void IndexDatabaseWriterContext::delete_chunk_statistics(int file_id) {
    std::vector<std::string> keys;
    scan_prefix(
        *db_, cf::CHUNK_STATS, prefix_for_file(file_id),
        [&](::rocksdb::Iterator& it) { keys.push_back(iterator_key(it)); });
    for (const auto& key : keys) {
        auto status = db_->del(batch_, cf::CHUNK_STATS, key);
        if (!status.ok()) {
            throw_db_error("Failed to delete chunk statistics", status);
        }
    }
}

void IndexDatabaseWriterContext::delete_file_contents(int file_id) {
    auto delete_prefix = [&](std::string_view cf_name,
                             std::string_view prefix) {
        std::vector<std::string> keys;
        scan_prefix(*db_, cf_name, prefix, [&](::rocksdb::Iterator& it) {
            keys.push_back(iterator_key(it));
        });
        for (const auto& key : keys) {
            auto del_status = db_->del(batch_, cf_name, key);
            if (!del_status.ok() && !del_status.IsNotFound()) {
                throw_db_error("Failed to delete file-scoped RocksDB data",
                               del_status);
            }
        }
    };

    auto delete_name_postings_by_owner = [&](std::string_view cf_name,
                                             int owner_fid,
                                             std::string_view prefix,
                                             bool chunk_level) {
        std::vector<std::string> owner_keys;
        scan_prefix(*db_, cf_name, prefix, [&](::rocksdb::Iterator& it) {
            owner_keys.push_back(iterator_key(it));
        });
        for (const auto& owner_key : owner_keys) {
            if (owner_key.size() < prefix.size() + 4) {
                continue;
            }
            const std::string_view payload(owner_key.data() + prefix.size(),
                                           owner_key.size() - prefix.size());
            if ((!chunk_level && payload.size() != 4) ||
                (chunk_level && payload.size() != 12)) {
                continue;
            }

            std::string primary_key("n|");
            primary_key.append(payload.data(), 4);
            rocks::KeyCodec::append_be32(primary_key,
                                         static_cast<std::uint32_t>(owner_fid));
            if (chunk_level) {
                primary_key.append(payload.data() + 4, payload.size() - 4);
            }

            auto del_one = [&](std::string_view key) {
                auto del_status = db_->del(batch_, cf_name, key);
                if (!del_status.ok() && !del_status.IsNotFound()) {
                    throw_db_error("Failed to delete exact name posting",
                                   del_status);
                }
            };

            del_one(primary_key);
            del_one(owner_key);
        }
    };

    delete_prefix(rocks::cf::MEMBERS, prefix_for_file(file_id));
    delete_prefix(rocks::cf::MEMBERS, gzip_member_prefix(file_id));
    delete_prefix(cf::METADATA, prefix_for_file(file_id));
    delete_prefix(cf::CHUNK_BLOOM, prefix_for_file(file_id));
    delete_prefix(cf::FILE_BLOOM, prefix_for_file(file_id));
    delete_prefix(cf::CHUNK_STATS, prefix_for_file(file_id));
    delete_prefix(cf::CHUNK_DIM_STATS, prefix_for_file(file_id));
    delete_prefix(cf::FILE_SCALAR_STATS, prefix_for_file(file_id));
    delete_prefix(cf::FILE_CAT_COUNTS, prefix_for_file(file_id));
    delete_prefix(cf::FILE_NAME_COUNTS, prefix_for_file(file_id));
    delete_prefix(cf::FILE_PID_TID_COUNTS, prefix_for_file(file_id));
    delete_name_postings_by_owner(cf::NAME_FILE_POSTINGS, file_id,
                                  name_file_owner_prefix(file_id), false);
    delete_name_postings_by_owner(cf::NAME_CHUNK_POSTINGS, file_id,
                                  name_chunk_owner_prefix(file_id), true);
    delete_prefix(cf::DIMENSIONS, std::string("d|") + prefix_for_file(file_id));
}

}  // namespace dftracer::utils::utilities::indexer
