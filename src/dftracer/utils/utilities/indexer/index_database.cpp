#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/rocksdb/key_codec.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/scan_prefix.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace dftracer::utils::utilities::indexer {

namespace queries = composites::dft::indexing::queries;
namespace rocks = dftracer::utils::rocksdb;

using internal::IndexerError;

namespace {

constexpr std::uint32_t kSchemaVersion = 1;

[[noreturn]] void throw_db_error(std::string_view message,
                                 const ::rocksdb::Status& status) {
    throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                       std::string(message) + ": " + status.ToString());
}

void append_u8(std::string& out, std::uint8_t value) {
    out.push_back(static_cast<char>(value));
}

void append_i64(std::string& out, std::int64_t value) {
    rocks::KeyCodec::append_be64(out, static_cast<std::uint64_t>(value));
}

void append_u64(std::string& out, std::uint64_t value) {
    rocks::KeyCodec::append_be64(out, value);
}

void append_double(std::string& out, double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u64(out, bits);
}

void append_string(std::string& out, std::string_view value) {
    rocks::KeyCodec::append_be32(out, static_cast<std::uint32_t>(value.size()));
    out.append(value.data(), value.size());
}

void append_blob(std::string& out, std::span<const unsigned char> blob) {
    rocks::KeyCodec::append_be32(out, static_cast<std::uint32_t>(blob.size()));
    out.append(reinterpret_cast<const char*>(blob.data()), blob.size());
}

class Cursor {
   public:
    explicit Cursor(std::string_view data) : data_(data) {}

    std::uint8_t u8() { return static_cast<std::uint8_t>(take(1)[0]); }

    std::uint32_t u32() { return rocks::KeyCodec::decode_be32(take(4)); }

    std::uint64_t u64() { return rocks::KeyCodec::decode_be64(take(8)); }

    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

    double f64() {
        std::uint64_t bits = u64();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    std::string str() {
        auto len = static_cast<std::size_t>(u32());
        auto bytes = take(len);
        return std::string(bytes.data(), bytes.size());
    }

    std::vector<unsigned char> blob() {
        auto len = static_cast<std::size_t>(u32());
        auto bytes = take(len);
        return std::vector<unsigned char>(bytes.begin(), bytes.end());
    }

   private:
    std::string_view take(std::size_t len) {
        if (offset_ + len > data_.size()) {
            throw std::runtime_error("Corrupt RocksDB payload");
        }
        auto chunk = data_.substr(offset_, len);
        offset_ += len;
        return chunk;
    }

    std::string_view data_;
    std::size_t offset_ = 0;
};

std::string file_lookup_key(std::string_view logical_name) {
    return std::string("f|") + std::string(logical_name);
}

std::string file_reverse_key(int file_id) {
    std::string key("r|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    return key;
}

std::string next_file_id_key() { return "_next_file_id"; }
std::string schema_version_key() { return "_schema_version"; }

std::string encode_file_record(int file_id, std::uint64_t file_hash) {
    std::string value;
    rocks::KeyCodec::append_be32(value, static_cast<std::uint32_t>(file_id));
    append_u64(value, 0);
    append_u64(value, 0);
    append_u64(value, file_hash);
    return value;
}

int decode_file_id(std::string_view record) {
    if (record.size() < 4) {
        throw std::runtime_error("Corrupt file record");
    }
    return static_cast<int>(rocks::KeyCodec::decode_be32(record.substr(0, 4)));
}

std::uint64_t decode_file_hash(std::string_view record) {
    if (record.size() < 28) {
        throw std::runtime_error("Corrupt file record");
    }
    return rocks::KeyCodec::decode_be64(record.substr(20, 8));
}

std::string prefix_for_file(int file_id) {
    return rocks::KeyCodec::encode_be32(static_cast<std::uint32_t>(file_id));
}

std::string make_hash_owner_key(int file_id, std::string_view dimension,
                                std::string_view hash_value) {
    std::string key("o|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    key.push_back('\0');
    key.append(dimension);
    key.push_back('\0');
    key.append(hash_value);
    return key;
}

std::string make_hash_forward_key(std::string_view dimension,
                                  std::string_view hash_value) {
    std::string key("h|");
    key.append(dimension);
    key.push_back('\0');
    key.append(hash_value);
    return key;
}

std::string make_hash_reverse_key(std::string_view dimension,
                                  std::string_view resolved_value,
                                  std::string_view hash_value) {
    std::string key("H|");
    key.append(dimension);
    key.push_back('\0');
    key.append(resolved_value);
    key.push_back('\0');
    key.append(hash_value);
    return key;
}

std::string make_dimension_key(int file_id, std::string_view dimension) {
    std::string key("d|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    key.append(dimension);
    return key;
}

std::string chunk_bloom_key(int file_id, std::string_view dimension,
                            std::uint64_t checkpoint_idx) {
    std::string key = prefix_for_file(file_id);
    key.append(dimension);
    key.push_back('\0');
    append_u64(key, checkpoint_idx);
    return key;
}

std::string file_bloom_key(int file_id, std::string_view dimension) {
    std::string key = prefix_for_file(file_id);
    key.append(dimension);
    return key;
}

std::string chunk_stats_key(int file_id, std::uint64_t checkpoint_idx) {
    std::string key = prefix_for_file(file_id);
    append_u64(key, checkpoint_idx);
    return key;
}

std::string checkpoint_key(int file_id, std::uint64_t uc_offset,
                           std::uint64_t checkpoint_idx) {
    std::string key = prefix_for_file(file_id);
    append_u64(key, uc_offset);
    append_u64(key, checkpoint_idx);
    return key;
}

std::string chunk_dim_stats_key(int file_id, std::uint64_t checkpoint_idx,
                                std::string_view dimension) {
    std::string key = prefix_for_file(file_id);
    append_u64(key, checkpoint_idx);
    key.append(dimension);
    return key;
}

std::string manifest_event_key(int file_id, std::uint64_t checkpoint_idx,
                               std::string_view cat, std::string_view name) {
    std::string key("E|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    append_u64(key, checkpoint_idx);
    key.append(cat);
    key.push_back('\0');
    key.append(name);
    return key;
}

std::string manifest_metadata_key(int file_id, std::uint64_t checkpoint_idx,
                                  std::string_view meta_type) {
    std::string key("M|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_id));
    append_u64(key, checkpoint_idx);
    key.append(meta_type);
    return key;
}

std::string metadata_key(int file_id) { return prefix_for_file(file_id); }

std::string tar_archive_key(int file_id) { return prefix_for_file(file_id); }

std::string tar_file_key(int file_id, std::uint64_t uncompressed_offset,
                         std::string_view file_name) {
    std::string key = prefix_for_file(file_id);
    append_u64(key, uncompressed_offset);
    key.push_back('\0');
    key.append(file_name);
    return key;
}

std::string encode_bloom_value(std::span<const unsigned char> blob,
                               std::uint64_t num_entries) {
    std::string value;
    append_u64(value, num_entries);
    value.append(reinterpret_cast<const char*>(blob.data()), blob.size());
    return value;
}

IndexDatabase::ChunkBloomResult decode_chunk_bloom(std::string_view key,
                                                   std::string_view value,
                                                   std::size_t prefix_size) {
    IndexDatabase::ChunkBloomResult result;
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

IndexDatabase::FileBloomResult decode_file_bloom(std::string_view value) {
    if (value.size() < 8) {
        throw std::runtime_error("Corrupt file bloom value");
    }
    IndexDatabase::FileBloomResult result;
    result.num_entries = rocks::KeyCodec::decode_be64(value.substr(0, 8));
    result.bloom_data.assign(value.begin() + 8, value.end());
    return result;
}

std::string encode_chunk_statistics_value(
    const IndexDatabase::ChunkStatistics& stats) {
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

    auto name_sketches = stats.serialize_name_duration_sketches();
    append_blob(value, name_sketches);
    append_string(value, stats.name_duration_histograms_json());
    append_string(value, stats.name_duration_sums_json());
    append_string(value, stats.name_duration_sum_sqs_json());
    append_string(value, stats.name_category_json());
    return value;
}

IndexDatabase::ChunkStatistics decode_chunk_statistics_value(
    std::string_view value) {
    Cursor cursor(value);
    IndexDatabase::ChunkStatistics stats;
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
            IndexDatabase::ChunkStatistics::deserialize_name_duration_sketches(
                name_sketches.data(), name_sketches.size());
    }

    stats.name_duration_histograms =
        IndexDatabase::ChunkStatistics::parse_histogram_map_json(cursor.str());
    stats.name_duration_sums =
        IndexDatabase::ChunkStatistics::parse_double_map_json(cursor.str());
    stats.name_duration_sum_sqs =
        IndexDatabase::ChunkStatistics::parse_double_map_json(cursor.str());
    stats.name_category =
        IndexDatabase::ChunkStatistics::parse_string_map_json(cursor.str());
    return stats;
}

std::string encode_checkpoint_value(
    const IndexDatabase::IndexerCheckpoint& checkpoint) {
    std::string value;
    append_u64(value, checkpoint.uc_size);
    append_u64(value, checkpoint.c_offset);
    append_u64(value, checkpoint.c_size);
    append_i64(value, checkpoint.bits);
    append_blob(value, checkpoint.dict_compressed);
    append_u64(value, checkpoint.num_lines);
    append_u64(value, checkpoint.first_line_num);
    append_u64(value, checkpoint.last_line_num);
    return value;
}

IndexDatabase::IndexerCheckpoint decode_checkpoint(std::string_view key,
                                                   std::string_view value) {
    if (key.size() < 20) {
        throw std::runtime_error("Corrupt checkpoint key");
    }

    IndexDatabase::IndexerCheckpoint checkpoint;
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

std::string encode_chunk_dimension_stats_value(
    const IndexDatabase::ChunkDimensionStats& stats,
    std::size_t value_counts_cap) {
    std::string value;
    append_u64(value, stats.distinct_count);
    append_string(value, stats.min_value);
    append_string(value, stats.max_value);
    append_string(value, stats.value_type);
    auto compressed = stats.compress_value_counts(value_counts_cap);
    append_u8(value, compressed.has_value() ? 1 : 0);
    if (compressed) {
        append_blob(value, *compressed);
    }
    return value;
}

IndexDatabase::ChunkDimensionStatsResult decode_chunk_dimension_stats_value(
    std::string_view key, std::string_view value) {
    IndexDatabase::ChunkDimensionStatsResult result;
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
        result.value_counts =
            IndexDatabase::ChunkDimensionStats::decompress_value_counts(
                compressed.data(), compressed.size());
    }
    return result;
}

std::string encode_event_range_value(std::span<const std::uint32_t> lines) {
    std::vector<std::uint32_t> vec(lines.begin(), lines.end());
    auto blob = queries::pack_line_numbers(vec);
    std::string value;
    append_u64(value, vec.size());
    append_blob(value, blob);
    return value;
}

std::vector<std::uint32_t> decode_line_numbers(Cursor& cursor) {
    auto blob = cursor.blob();
    return queries::unpack_line_numbers(blob.data(), blob.size());
}

std::string encode_metadata_value(std::span<const std::uint32_t> lines) {
    std::vector<std::uint32_t> vec(lines.begin(), lines.end());
    auto blob = queries::pack_line_numbers(vec);
    std::string value;
    append_blob(value, blob);
    return value;
}

std::string encode_metadata_record(std::uint64_t checkpoint_size,
                                   std::uint64_t total_lines,
                                   std::uint64_t total_uc_size) {
    std::string value;
    append_u64(value, checkpoint_size);
    append_u64(value, total_lines);
    append_u64(value, total_uc_size);
    return value;
}

std::string encode_tar_archive_value(std::string_view archive_name,
                                     std::uint64_t checkpoint_size,
                                     std::uint64_t total_lines,
                                     std::uint64_t total_uc_size,
                                     std::uint64_t total_files) {
    std::string value;
    append_string(value, archive_name);
    append_u64(value, checkpoint_size);
    append_u64(value, total_lines);
    append_u64(value, total_uc_size);
    append_u64(value, total_files);
    return value;
}

IndexDatabase::TarArchiveMetadata decode_tar_archive_value(
    std::string_view value) {
    Cursor cursor(value);
    IndexDatabase::TarArchiveMetadata metadata;
    metadata.archive_name = cursor.str();
    metadata.checkpoint_size = cursor.u64();
    metadata.total_lines = cursor.u64();
    metadata.total_uc_size = cursor.u64();
    metadata.total_files = cursor.u64();
    return metadata;
}

std::string encode_tar_file_value(const IndexDatabase::TarFileRecord& record) {
    std::string value;
    append_u64(value, record.file_size);
    append_u64(value, record.file_mtime);
    append_u8(value, static_cast<std::uint8_t>(record.typeflag));
    append_u64(value, record.data_offset);
    return value;
}

IndexDatabase::TarFileRecord decode_tar_file(std::string_view key,
                                             std::string_view value) {
    if (key.size() < 13) {
        throw std::runtime_error("Corrupt tar file key");
    }

    const auto name_pos = key.find('\0', 12);
    if (name_pos == std::string_view::npos) {
        throw std::runtime_error("Corrupt tar file key");
    }

    Cursor cursor(value);
    IndexDatabase::TarFileRecord record;
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

IndexDatabase::IndexDatabase(const std::string& index_path,
                             rocks::RocksDatabase::OpenMode open_mode)
    : db_path_(internal::normalize_index_root(index_path)),
      open_mode_(open_mode),
      db_(rocks::RocksDBManager::instance().get_or_open(db_path_, open_mode_)) {
    if (open_mode_ == rocks::RocksDatabase::OpenMode::ReadWrite) {
        init_base_schema();
    }
}

void IndexDatabase::init_base_schema() {
    std::string value;
    auto status = db_->get(schema_version_key(), &value);
    if (status.IsNotFound()) {
        status = db_->put(schema_version_key(),
                          rocks::KeyCodec::encode_be32(kSchemaVersion));
        if (!status.ok()) {
            throw_db_error("Failed to initialize schema version", status);
        }
    } else if (!status.ok()) {
        throw_db_error("Failed to read schema version", status);
    }
}

void IndexDatabase::init_bloom_schema() {
    // RocksDB column families are provisioned at DB open; bloom-specific
    // schema initialization is intentionally a no-op.
}

void IndexDatabase::init_manifest_schema() {
    // RocksDB column families are provisioned at DB open; manifest-specific
    // schema initialization is intentionally a no-op.
}

bool IndexDatabase::has_bloom_data(int file_id) const {
    bool found = false;
    auto prefix = prefix_for_file(file_id);
    scan_prefix(*db_, "chunk_bloom", prefix,
                [&found](::rocksdb::Iterator&) { found = true; });
    return found;
}

bool IndexDatabase::has_manifest_data(int file_id) const {
    bool found = false;
    std::string prefix("E|");
    rocks::KeyCodec::append_be32(prefix, static_cast<std::uint32_t>(file_id));
    scan_prefix(*db_, "manifest", prefix,
                [&found](::rocksdb::Iterator&) { found = true; });
    return found;
}

int IndexDatabase::get_or_create_file_info(std::string_view path,
                                           std::uint64_t file_hash) {
    const auto logical_name = std::string(path);
    const auto lookup = file_lookup_key(logical_name);
    std::string existing;
    auto status = db_->get(lookup, &existing);
    if (status.ok()) {
        const auto file_id = decode_file_id(existing);
        if (decode_file_hash(existing) == file_hash) {
            return file_id;
        }
        delete_file_data(file_id);
        auto registry = encode_file_record(file_id, file_hash);
        if (txn_batch_) {
            status = db_->put(*txn_batch_, "default", lookup, registry);
            if (!status.ok()) {
                throw_db_error("Failed to update file registry", status);
            }
            status = db_->put(*txn_batch_, "default", file_reverse_key(file_id),
                              logical_name);
            if (!status.ok()) {
                throw_db_error("Failed to update reverse file registry",
                               status);
            }
        } else {
            status = db_->put(lookup, registry);
            if (!status.ok()) {
                throw_db_error("Failed to update file registry", status);
            }
            status = db_->put(file_reverse_key(file_id), logical_name);
            if (!status.ok()) {
                throw_db_error("Failed to update reverse file registry",
                               status);
            }
        }
        return file_id;
    }
    if (!status.IsNotFound()) {
        throw_db_error("Failed to query file registry", status);
    }

    std::uint32_t next_id = 1;
    std::string next_value;
    status = db_->get(next_file_id_key(), &next_value);
    if (status.ok()) {
        next_id = rocks::KeyCodec::decode_be32(next_value);
    } else if (!status.IsNotFound()) {
        throw_db_error("Failed to read next file id", status);
    }

    const auto file_id = static_cast<int>(next_id);
    const auto new_registry = encode_file_record(file_id, file_hash);
    const auto next_registry = rocks::KeyCodec::encode_be32(next_id + 1);

    if (txn_batch_) {
        status = db_->put(*txn_batch_, "default", lookup, new_registry);
        if (!status.ok()) {
            throw_db_error("Failed to insert file registry", status);
        }
        status = db_->put(*txn_batch_, "default", file_reverse_key(file_id),
                          logical_name);
        if (!status.ok()) {
            throw_db_error("Failed to insert reverse file registry", status);
        }
        status =
            db_->put(*txn_batch_, "default", next_file_id_key(), next_registry);
        if (!status.ok()) {
            throw_db_error("Failed to update next file id", status);
        }
    } else {
        status = db_->put(lookup, new_registry);
        if (!status.ok()) {
            throw_db_error("Failed to insert file registry", status);
        }
        status = db_->put(file_reverse_key(file_id), logical_name);
        if (!status.ok()) {
            throw_db_error("Failed to insert reverse file registry", status);
        }
        status = db_->put(next_file_id_key(), next_registry);
        if (!status.ok()) {
            throw_db_error("Failed to update next file id", status);
        }
    }

    return file_id;
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

int IndexDatabase::find_file(std::string_view file_path) const {
    return get_file_info_id(internal::get_logical_path(file_path));
}

void IndexDatabase::begin_transaction() {
    txn_batch_ =
        std::make_unique<rocks::RocksDatabase::Batch>(db_->begin_batch());
}

void IndexDatabase::commit_transaction() {
    if (!txn_batch_) {
        return;
    }
    auto status = db_->commit_batch(*txn_batch_);
    txn_batch_.reset();
    if (!status.ok()) {
        throw_db_error("Failed to commit RocksDB batch", status);
    }
}

void IndexDatabase::rollback_transaction() noexcept { txn_batch_.reset(); }

void IndexDatabase::insert_chunk_bloom_filter(
    int file_id, std::uint64_t checkpoint_idx, std::string_view dimension,
    std::span<const unsigned char> blob_data, std::uint64_t num_entries) {
    const auto key = chunk_bloom_key(file_id, dimension, checkpoint_idx);
    const auto value = encode_bloom_value(blob_data, num_entries);
    auto status = txn_batch_ ? db_->put(*txn_batch_, "chunk_bloom", key, value)
                             : db_->put(key, value, "chunk_bloom");
    if (!status.ok()) {
        throw_db_error("Failed to insert chunk bloom filter", status);
    }
}

void IndexDatabase::insert_chunk_bloom_filter(
    int file_id, std::uint64_t checkpoint_idx, std::string_view dimension,
    const void* blob_data, int blob_size, std::uint64_t num_entries) {
    auto* bytes = static_cast<const unsigned char*>(blob_data);
    insert_chunk_bloom_filter(file_id, checkpoint_idx, dimension,
                              std::span<const unsigned char>(
                                  bytes, static_cast<std::size_t>(blob_size)),
                              num_entries);
}

void IndexDatabase::insert_file_bloom_filter(
    int file_id, std::string_view dimension,
    std::span<const unsigned char> blob_data, std::uint64_t num_entries) {
    const auto key = file_bloom_key(file_id, dimension);
    const auto value = encode_bloom_value(blob_data, num_entries);
    auto status = txn_batch_ ? db_->put(*txn_batch_, "file_bloom", key, value)
                             : db_->put(key, value, "file_bloom");
    if (!status.ok()) {
        throw_db_error("Failed to insert file bloom filter", status);
    }
}

void IndexDatabase::insert_file_bloom_filter(int file_id,
                                             std::string_view dimension,
                                             const void* blob_data,
                                             int blob_size,
                                             std::uint64_t num_entries) {
    auto* bytes = static_cast<const unsigned char*>(blob_data);
    insert_file_bloom_filter(file_id, dimension,
                             std::span<const unsigned char>(
                                 bytes, static_cast<std::size_t>(blob_size)),
                             num_entries);
}

void IndexDatabase::insert_chunk_statistics(int file_id,
                                            std::uint64_t checkpoint_idx,
                                            const ChunkStatistics& stats) {
    const auto key = chunk_stats_key(file_id, checkpoint_idx);
    const auto value = encode_chunk_statistics_value(stats);
    auto status = txn_batch_ ? db_->put(*txn_batch_, "chunk_stats", key, value)
                             : db_->put(key, value, "chunk_stats");
    if (!status.ok()) {
        throw_db_error("Failed to insert chunk statistics", status);
    }
}

void IndexDatabase::insert_checkpoint(int file_id,
                                      const IndexerCheckpoint& checkpoint) {
    const auto key = checkpoint_key(file_id, checkpoint.uc_offset,
                                    checkpoint.checkpoint_idx);
    const auto value = encode_checkpoint_value(checkpoint);
    auto status = txn_batch_ ? db_->put(*txn_batch_, "checkpoints", key, value)
                             : db_->put(key, value, "checkpoints");
    if (!status.ok()) {
        throw_db_error("Failed to insert checkpoint", status);
    }
}

void IndexDatabase::insert_index_dimension(int file_id,
                                           std::string_view dimension) {
    const auto key = make_dimension_key(file_id, dimension);
    auto status = txn_batch_ ? db_->put(*txn_batch_, "dimensions", key, "")
                             : db_->put(key, "", "dimensions");
    if (!status.ok()) {
        throw_db_error("Failed to insert index dimension", status);
    }
}

void IndexDatabase::insert_hash_resolution(int file_id,
                                           std::string_view dimension,
                                           std::string_view hash_value,
                                           std::string_view resolved_value) {
    const auto owner = make_hash_owner_key(file_id, dimension, hash_value);
    const auto forward = make_hash_forward_key(dimension, hash_value);
    const auto reverse =
        make_hash_reverse_key(dimension, resolved_value, hash_value);
    if (txn_batch_) {
        db_->put(*txn_batch_, "dimensions", owner, std::string(resolved_value));
        db_->put(*txn_batch_, "dimensions", forward,
                 std::string(resolved_value));
        db_->put(*txn_batch_, "dimensions", reverse, "");
        return;
    }
    auto status = db_->put(owner, resolved_value, "dimensions");
    if (!status.ok()) throw_db_error("Failed to insert hash owner", status);
    status = db_->put(forward, resolved_value, "dimensions");
    if (!status.ok())
        throw_db_error("Failed to insert hash resolution", status);
    status = db_->put(reverse, "", "dimensions");
    if (!status.ok()) {
        throw_db_error("Failed to insert reverse hash resolution", status);
    }
}

void IndexDatabase::insert_chunk_dimension_stats(
    int file_id, std::uint64_t checkpoint_idx, const ChunkDimensionStats& stats,
    std::size_t value_counts_cap) {
    const auto key =
        chunk_dim_stats_key(file_id, checkpoint_idx, stats.dimension);
    const auto value =
        encode_chunk_dimension_stats_value(stats, value_counts_cap);
    auto status = txn_batch_
                      ? db_->put(*txn_batch_, "chunk_dim_stats", key, value)
                      : db_->put(key, value, "chunk_dim_stats");
    if (!status.ok()) {
        throw_db_error("Failed to insert chunk dimension stats", status);
    }
}

void IndexDatabase::insert_tar_archive_metadata(int file_id,
                                                std::string_view archive_name,
                                                std::uint64_t checkpoint_size,
                                                std::uint64_t total_lines,
                                                std::uint64_t total_uc_size,
                                                std::uint64_t total_files) {
    const auto key = tar_archive_key(file_id);
    const auto value = encode_tar_archive_value(
        archive_name, checkpoint_size, total_lines, total_uc_size, total_files);
    auto status = txn_batch_ ? db_->put(*txn_batch_, "archives", key, value)
                             : db_->put(key, value, "archives");
    if (!status.ok()) {
        throw_db_error("Failed to insert tar archive metadata", status);
    }
}

void IndexDatabase::insert_tar_file(int file_id, const TarFileRecord& record) {
    const auto key =
        tar_file_key(file_id, record.uncompressed_offset, record.file_name);
    const auto value = encode_tar_file_value(record);
    auto status = txn_batch_ ? db_->put(*txn_batch_, "tar_files", key, value)
                             : db_->put(key, value, "tar_files");
    if (!status.ok()) {
        throw_db_error("Failed to insert tar file metadata", status);
    }
}

std::vector<IndexDatabase::ChunkBloomResult>
IndexDatabase::query_chunk_bloom_filters(int file_id,
                                         std::string_view dimension) const {
    std::vector<ChunkBloomResult> results;
    std::string prefix = prefix_for_file(file_id);
    prefix.append(dimension);
    prefix.push_back('\0');
    scan_prefix(*db_, "chunk_bloom", prefix, [&](::rocksdb::Iterator& it) {
        results.push_back(decode_chunk_bloom(
            iterator_key(it), iterator_value(it), prefix.size() - 1));
    });
    return results;
}

std::unordered_map<std::string, std::vector<IndexDatabase::ChunkBloomResult>>
IndexDatabase::query_chunk_bloom_filters_batch(
    int file_id, const std::vector<std::string>& dimensions) const {
    std::unordered_map<std::string, std::vector<ChunkBloomResult>> results;
    for (const auto& dimension : dimensions) {
        results.emplace(dimension,
                        query_chunk_bloom_filters(file_id, dimension));
    }
    return results;
}

std::optional<IndexDatabase::FileBloomResult>
IndexDatabase::query_file_bloom_filter(int file_id,
                                       std::string_view dimension) const {
    std::string value;
    auto status =
        db_->get(file_bloom_key(file_id, dimension), &value, "file_bloom");
    if (status.IsNotFound()) {
        return std::nullopt;
    }
    if (!status.ok()) {
        throw_db_error("Failed to query file bloom filter", status);
    }
    return decode_file_bloom(value);
}

std::unordered_map<std::string, IndexDatabase::FileBloomResult>
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
    scan_prefix(*db_, "dimensions", prefix, [&](::rocksdb::Iterator& it) {
        auto key = iterator_key(it);
        dimensions.push_back(key.substr(prefix.size()));
    });
    return dimensions;
}

bool IndexDatabase::has_index_dimension(int file_id,
                                        std::string_view dimension) const {
    std::string value;
    return db_
        ->get(make_dimension_key(file_id, dimension), &value, "dimensions")
        .ok();
}

std::vector<IndexDatabase::ChunkStatisticsResult>
IndexDatabase::query_chunk_statistics(int file_id) const {
    std::vector<ChunkStatisticsResult> results;
    const auto prefix = prefix_for_file(file_id);
    scan_prefix(*db_, "chunk_stats", prefix, [&](::rocksdb::Iterator& it) {
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

bool IndexDatabase::find_checkpoint(int file_id, std::size_t target_offset,
                                    IndexerCheckpoint& checkpoint) const {
    if (target_offset == 0 || file_id < 0) {
        return false;
    }

    bool found = false;
    const auto prefix = prefix_for_file(file_id);
    scan_prefix(*db_, "checkpoints", prefix, [&](::rocksdb::Iterator& it) {
        auto decoded = decode_checkpoint(iterator_key(it), iterator_value(it));
        if (decoded.uc_offset <= target_offset &&
            (!found || decoded.uc_offset >= checkpoint.uc_offset)) {
            checkpoint = std::move(decoded);
            found = true;
        }
    });
    return found;
}

std::vector<IndexDatabase::IndexerCheckpoint> IndexDatabase::query_checkpoints(
    int file_id) const {
    std::vector<IndexerCheckpoint> checkpoints;
    const auto prefix = prefix_for_file(file_id);
    scan_prefix(*db_, "checkpoints", prefix, [&](::rocksdb::Iterator& it) {
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

std::optional<IndexDatabase::TarArchiveMetadata>
IndexDatabase::query_tar_archive_metadata(int file_id) const {
    std::string value;
    auto status = db_->get(tar_archive_key(file_id), &value, "archives");
    if (status.IsNotFound()) {
        return std::nullopt;
    }
    if (!status.ok()) {
        throw_db_error("Failed to read tar archive metadata", status);
    }
    return decode_tar_archive_value(value);
}

std::vector<IndexDatabase::TarFileRecord> IndexDatabase::query_tar_files(
    int file_id) const {
    std::vector<TarFileRecord> files;
    const auto prefix = prefix_for_file(file_id);
    scan_prefix(*db_, "tar_files", prefix, [&](::rocksdb::Iterator& it) {
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

std::vector<IndexDatabase::TarFileRecord>
IndexDatabase::query_tar_files_in_range(int file_id, std::uint64_t start_offset,
                                        std::uint64_t end_offset) const {
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

std::vector<IndexDatabase::IndexerCheckpoint>
IndexDatabase::query_checkpoints_for_line_range(int file_id,
                                                std::uint64_t start_line,
                                                std::uint64_t end_line) const {
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

IndexDatabase::TimeBounds IndexDatabase::query_time_bounds(int file_id) const {
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

std::vector<IndexDatabase::ChunkDimensionStatsResult>
IndexDatabase::query_chunk_dimension_stats(int file_id) const {
    std::vector<ChunkDimensionStatsResult> results;
    const auto prefix = prefix_for_file(file_id);
    scan_prefix(*db_, "chunk_dim_stats", prefix, [&](::rocksdb::Iterator& it) {
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

std::vector<IndexDatabase::ChunkDimensionStatsResult>
IndexDatabase::query_chunk_dimension_stats_for_dimension(
    int file_id, std::string_view dimension) const {
    std::vector<ChunkDimensionStatsResult> results;
    const auto prefix = prefix_for_file(file_id);
    scan_prefix(*db_, "chunk_dim_stats", prefix, [&](::rocksdb::Iterator& it) {
        auto decoded = decode_chunk_dimension_stats_value(iterator_key(it),
                                                          iterator_value(it));
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

std::optional<std::string> IndexDatabase::query_resolved_by_hash(
    std::string_view dimension, std::string_view hash_value) const {
    std::string value;
    auto status = db_->get(make_hash_forward_key(dimension, hash_value), &value,
                           "dimensions");
    if (status.IsNotFound()) {
        return std::nullopt;
    }
    if (!status.ok()) {
        throw_db_error("Failed to query resolved hash", status);
    }
    return value;
}

std::vector<std::string> IndexDatabase::query_hash_by_resolved(
    std::string_view dimension, std::string_view resolved_value) const {
    std::vector<std::string> hashes;
    auto prefix = make_hash_reverse_key(dimension, resolved_value, "");
    scan_prefix(*db_, "dimensions", prefix, [&](::rocksdb::Iterator& it) {
        auto key = iterator_key(it);
        hashes.push_back(key.substr(prefix.size()));
    });
    return hashes;
}

void IndexDatabase::delete_chunk_bloom_filters(int file_id,
                                               std::string_view dimension) {
    std::vector<std::string> keys;
    std::string prefix = prefix_for_file(file_id);
    prefix.append(dimension);
    prefix.push_back('\0');
    scan_prefix(*db_, "chunk_bloom", prefix, [&](::rocksdb::Iterator& it) {
        keys.push_back(iterator_key(it));
    });
    for (const auto& key : keys) {
        auto status = txn_batch_ ? db_->del(*txn_batch_, "chunk_bloom", key)
                                 : db_->del(key, "chunk_bloom");
        if (!status.ok())
            throw_db_error("Failed to delete chunk bloom", status);
    }
}

void IndexDatabase::delete_file_bloom_filter(int file_id,
                                             std::string_view dimension) {
    auto status =
        txn_batch_ ? db_->del(*txn_batch_, "file_bloom",
                              file_bloom_key(file_id, dimension))
                   : db_->del(file_bloom_key(file_id, dimension), "file_bloom");
    if (!status.ok() && !status.IsNotFound()) {
        throw_db_error("Failed to delete file bloom", status);
    }
}

void IndexDatabase::delete_chunk_statistics(int file_id) {
    std::vector<std::string> keys;
    scan_prefix(
        *db_, "chunk_stats", prefix_for_file(file_id),
        [&](::rocksdb::Iterator& it) { keys.push_back(iterator_key(it)); });
    for (const auto& key : keys) {
        auto status = txn_batch_ ? db_->del(*txn_batch_, "chunk_stats", key)
                                 : db_->del(key, "chunk_stats");
        if (!status.ok()) {
            throw_db_error("Failed to delete chunk statistics", status);
        }
    }
}

void IndexDatabase::delete_chunk_dimension_stats(int file_id) {
    std::vector<std::string> keys;
    scan_prefix(
        *db_, "chunk_dim_stats", prefix_for_file(file_id),
        [&](::rocksdb::Iterator& it) { keys.push_back(iterator_key(it)); });
    for (const auto& key : keys) {
        auto status = txn_batch_ ? db_->del(*txn_batch_, "chunk_dim_stats", key)
                                 : db_->del(key, "chunk_dim_stats");
        if (!status.ok()) {
            throw_db_error("Failed to delete chunk dimension stats", status);
        }
    }
}

void IndexDatabase::delete_hash_resolutions(int file_id) {
    std::vector<std::pair<std::string, std::string>> owned;
    std::string prefix("o|");
    rocks::KeyCodec::append_be32(prefix, static_cast<std::uint32_t>(file_id));
    prefix.push_back('\0');
    scan_prefix(*db_, "dimensions", prefix, [&](::rocksdb::Iterator& it) {
        owned.emplace_back(iterator_key(it), iterator_value(it));
    });
    for (const auto& [owner_key, resolved] : owned) {
        if (owner_key.size() <= prefix.size()) {
            DFTRACER_UTILS_LOG_WARN(
                "Skipping malformed owner key for file_id=%d", file_id);
            continue;
        }
        const std::string_view payload(owner_key.data() + prefix.size(),
                                       owner_key.size() - prefix.size());
        auto split = payload.find('\0');
        if (split == std::string_view::npos) {
            DFTRACER_UTILS_LOG_WARN(
                "Skipping malformed owner key payload for file_id=%d", file_id);
            continue;
        }
        auto dimension = payload.substr(0, split);
        auto hash_value = payload.substr(split + 1);
        auto forward = make_hash_forward_key(dimension, hash_value);
        auto reverse = make_hash_reverse_key(dimension, resolved, hash_value);
        const auto del_one = [&](std::string_view key) {
            auto status = txn_batch_ ? db_->del(*txn_batch_, "dimensions", key)
                                     : db_->del(key, "dimensions");
            if (!status.ok() && !status.IsNotFound()) {
                throw_db_error("Failed to delete hash resolution", status);
            }
        };
        del_one(owner_key);
        del_one(forward);
        del_one(reverse);
    }
}

void IndexDatabase::insert_event_range(
    int file_id, std::uint64_t checkpoint_idx, std::string_view cat,
    std::string_view name, std::span<const std::uint32_t> line_numbers) {
    const auto key = manifest_event_key(file_id, checkpoint_idx, cat, name);
    const auto value = encode_event_range_value(line_numbers);
    auto status = txn_batch_ ? db_->put(*txn_batch_, "manifest", key, value)
                             : db_->put(key, value, "manifest");
    if (!status.ok()) {
        throw_db_error("Failed to insert event range", status);
    }
}

void IndexDatabase::insert_event_range(
    int file_id, std::uint64_t checkpoint_idx, std::string_view cat,
    std::string_view name, const std::vector<std::uint32_t>& line_numbers) {
    insert_event_range(file_id, checkpoint_idx, cat, name,
                       std::span<const std::uint32_t>(line_numbers));
}

void IndexDatabase::insert_metadata_lines(
    int file_id, std::uint64_t checkpoint_idx, std::string_view meta_type,
    std::span<const std::uint32_t> line_numbers) {
    const auto key = manifest_metadata_key(file_id, checkpoint_idx, meta_type);
    const auto value = encode_metadata_value(line_numbers);
    auto status = txn_batch_ ? db_->put(*txn_batch_, "manifest", key, value)
                             : db_->put(key, value, "manifest");
    if (!status.ok()) {
        throw_db_error("Failed to insert metadata lines", status);
    }
}

void IndexDatabase::insert_metadata_lines(
    int file_id, std::uint64_t checkpoint_idx, std::string_view meta_type,
    const std::vector<std::uint32_t>& line_numbers) {
    insert_metadata_lines(file_id, checkpoint_idx, meta_type,
                          std::span<const std::uint32_t>(line_numbers));
}

std::vector<IndexDatabase::EventRangeResult> IndexDatabase::query_event_ranges(
    int file_id) const {
    std::vector<EventRangeResult> results;
    std::string prefix("E|");
    rocks::KeyCodec::append_be32(prefix, static_cast<std::uint32_t>(file_id));
    scan_prefix(*db_, "manifest", prefix, [&](::rocksdb::Iterator& it) {
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

std::vector<IndexDatabase::EventRangeResult>
IndexDatabase::query_event_ranges_for_checkpoint(
    int file_id, std::uint64_t checkpoint_idx) const {
    std::vector<EventRangeResult> results;
    for (auto& range : query_event_ranges(file_id)) {
        if (range.checkpoint_idx == checkpoint_idx) {
            results.push_back(std::move(range));
        }
    }
    return results;
}

std::vector<IndexDatabase::MetadataLinesResult>
IndexDatabase::query_metadata_lines(int file_id) const {
    std::vector<MetadataLinesResult> results;
    std::string prefix("M|");
    rocks::KeyCodec::append_be32(prefix, static_cast<std::uint32_t>(file_id));
    scan_prefix(*db_, "manifest", prefix, [&](::rocksdb::Iterator& it) {
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

std::vector<IndexDatabase::MetadataLinesResult>
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

void IndexDatabase::delete_event_ranges(int file_id) {
    std::vector<std::string> keys;
    std::string prefix("E|");
    rocks::KeyCodec::append_be32(prefix, static_cast<std::uint32_t>(file_id));
    scan_prefix(*db_, "manifest", prefix, [&](::rocksdb::Iterator& it) {
        keys.push_back(iterator_key(it));
    });
    for (const auto& key : keys) {
        auto status = txn_batch_ ? db_->del(*txn_batch_, "manifest", key)
                                 : db_->del(key, "manifest");
        if (!status.ok()) {
            throw_db_error("Failed to delete manifest event ranges", status);
        }
    }
}

void IndexDatabase::delete_metadata_lines(int file_id) {
    std::vector<std::string> keys;
    std::string prefix("M|");
    rocks::KeyCodec::append_be32(prefix, static_cast<std::uint32_t>(file_id));
    scan_prefix(*db_, "manifest", prefix, [&](::rocksdb::Iterator& it) {
        keys.push_back(iterator_key(it));
    });
    for (const auto& key : keys) {
        auto status = txn_batch_ ? db_->del(*txn_batch_, "manifest", key)
                                 : db_->del(key, "manifest");
        if (!status.ok()) {
            throw_db_error("Failed to delete metadata lines", status);
        }
    }
}

std::uint64_t IndexDatabase::get_total_events(int file_id) const {
    std::uint64_t total = 0;
    for (const auto& row : query_chunk_statistics(file_id)) {
        total += row.stats.total_events;
    }
    return total > 0 ? total : get_num_lines(file_id);
}

void IndexDatabase::insert_file_metadata(int file_id,
                                         std::uint64_t checkpoint_size,
                                         std::uint64_t total_lines,
                                         std::uint64_t total_uc_size) {
    const auto key = metadata_key(file_id);
    const auto value =
        encode_metadata_record(checkpoint_size, total_lines, total_uc_size);
    auto status = txn_batch_ ? db_->put(*txn_batch_, "metadata", key, value)
                             : db_->put(key, value, "metadata");
    if (!status.ok()) {
        throw_db_error("Failed to insert metadata", status);
    }
}

std::uint64_t IndexDatabase::get_checkpoint_size(int file_id) const {
    std::string value;
    auto status = db_->get(metadata_key(file_id), &value, "metadata");
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
    auto status = db_->get(metadata_key(file_id), &value, "metadata");
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
    auto status = db_->get(metadata_key(file_id), &value, "metadata");
    if (status.IsNotFound()) {
        return 0;
    }
    if (!status.ok()) {
        throw_db_error("Failed to read metadata", status);
    }
    return decode_metadata_record(value)[2];
}

void IndexDatabase::delete_file_data(int file_id) {
    auto delete_default_key = [&](std::string_view key) {
        auto del_status =
            txn_batch_ ? db_->del(*txn_batch_, "default", key) : db_->del(key);
        if (!del_status.ok() && !del_status.IsNotFound()) {
            throw_db_error("Failed to delete file registry entry", del_status);
        }
    };

    const auto logical_name_key = file_reverse_key(file_id);
    std::string logical_name;
    auto status = db_->get(logical_name_key, &logical_name);
    if (status.ok()) {
        delete_default_key(file_lookup_key(logical_name));
        delete_default_key(logical_name_key);
    } else if (!status.IsNotFound()) {
        throw_db_error("Failed to read reverse file registry", status);
    }

    auto delete_prefix = [&](std::string_view cf, std::string_view prefix) {
        std::vector<std::string> keys;
        scan_prefix(*db_, cf, prefix, [&](::rocksdb::Iterator& it) {
            keys.push_back(iterator_key(it));
        });
        for (const auto& key : keys) {
            auto del_status =
                txn_batch_ ? db_->del(*txn_batch_, cf, key) : db_->del(key, cf);
            if (!del_status.ok() && !del_status.IsNotFound()) {
                throw_db_error("Failed to delete file-scoped RocksDB data",
                               del_status);
            }
        }
    };

    delete_prefix("checkpoints", prefix_for_file(file_id));
    delete_prefix("metadata", prefix_for_file(file_id));
    delete_prefix("archives", prefix_for_file(file_id));
    delete_prefix("tar_files", prefix_for_file(file_id));
    delete_prefix("chunk_bloom", prefix_for_file(file_id));
    delete_prefix("file_bloom", prefix_for_file(file_id));
    delete_prefix("chunk_stats", prefix_for_file(file_id));
    delete_prefix("chunk_dim_stats", prefix_for_file(file_id));
    delete_prefix("dimensions", std::string("d|") + prefix_for_file(file_id));
    delete_prefix("manifest", std::string("E|") + prefix_for_file(file_id));
    delete_prefix("manifest", std::string("M|") + prefix_for_file(file_id));
    delete_hash_resolutions(file_id);
}

}  // namespace dftracer::utils::utilities::indexer
