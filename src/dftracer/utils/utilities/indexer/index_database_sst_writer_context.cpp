#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/utilities/indexer/error.h>
#include <dftracer/utils/utilities/indexer/index_database_sst_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/db_error.h>
#include <dftracer/utils/utilities/indexer/internal/index_encoding.h>
#include <dftracer/utils/utilities/indexer/internal/statistics_codec.h>
#include <rocksdb/sst_file_writer.h>

#include <algorithm>
#include <stdexcept>

namespace dftracer::utils::utilities::indexer {

namespace {

namespace encoding = internal::encoding;

std::string emit_sst(const std::string& path,
                     std::vector<std::pair<std::string, std::string>>& buffer) {
    std::sort(buffer.begin(), buffer.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    // SstFileWriter requires strict ascending keys. Buffers like
    // name_dictionary and hash_tables collect duplicate (key, value) pairs
    // when a batch spans multiple files that share event names / content
    // hashes. Drop consecutive duplicates so the writer sees unique keys.
    buffer.erase(std::unique(buffer.begin(), buffer.end(),
                             [](const auto& a, const auto& b) {
                                 return a.first == b.first;
                             }),
                 buffer.end());

    ::rocksdb::EnvOptions env_opts;
    ::rocksdb::Options writer_options(
        rocksdb::RocksDatabase::default_options(),
        rocksdb::RocksDatabase::default_column_family_options());
    ::rocksdb::SstFileWriter writer(env_opts, writer_options);

    auto status = writer.Open(path);
    if (!status.ok()) {
        throw_db_error("Failed to open SST writer at '" + path + "'", status);
    }

    for (const auto& [key, value] : buffer) {
        status = writer.Put(key, value);
        if (!status.ok()) {
            throw_db_error("Failed to append to SST '" + path + "'", status);
        }
    }

    status = writer.Finish();
    if (!status.ok()) {
        throw_db_error("Failed to finalize SST '" + path + "'", status);
    }

    return path;
}

/// Emit a mixed Put+Merge SST for the AGGREGATION / SYSTEM_METRICS CFs.
/// Entries are sorted by key; each entry's `is_merge` decides whether to
/// call SstFileWriter::Merge (operand, combined by merge_operator at
/// read/compaction time) or ::Put (overrides prior values).
std::string emit_mixed_sst(
    const std::string& path,
    std::vector<IndexDatabaseSstWriterContext::MergeableKeyValue>& buffer) {
    std::sort(buffer.begin(), buffer.end(),
              [](const auto& a, const auto& b) { return a.key < b.key; });

    ::rocksdb::EnvOptions env_opts;
    ::rocksdb::Options writer_options(
        rocksdb::RocksDatabase::default_options(),
        rocksdb::RocksDatabase::default_column_family_options());
    ::rocksdb::SstFileWriter writer(env_opts, writer_options);

    auto status = writer.Open(path);
    if (!status.ok()) {
        throw_db_error("Failed to open SST writer at '" + path + "'", status);
    }
    for (const auto& entry : buffer) {
        status = entry.is_merge ? writer.Merge(entry.key, entry.value)
                                : writer.Put(entry.key, entry.value);
        if (!status.ok()) {
            throw_db_error("Failed to append to SST '" + path + "'", status);
        }
    }
    status = writer.Finish();
    if (!status.ok()) {
        throw_db_error("Failed to finalize SST '" + path + "'", status);
    }
    return path;
}

}  // namespace

namespace {

/// Move one file from `src` to `dst`. Uses rename (O(1) same-FS) with a
/// copy+unlink fallback for cross-FS. `dst` parent directory must exist.
void move_file(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::rename(src, dst, ec);
    if (!ec) return;
    // Cross-FS or other rename failure -> fall back to copy.
    ec.clear();
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        throw IndexerError(IndexerError::Type::FILE_ERROR,
                           "Failed to move SST '" + src.string() + "' to '" +
                               dst.string() + "': " + ec.message());
    }
    fs::remove(src, ec);  // best-effort; staging cleanup handled by caller
}

void move_one(const fs::path& dest_dir, std::optional<std::string>& src_slot,
              std::optional<std::string>& dst_slot) {
    if (!src_slot.has_value()) return;
    fs::path src(*src_slot);
    fs::path dst = dest_dir / src.filename();
    move_file(src, dst);
    dst_slot = dst.string();
    src_slot.reset();
}

}  // namespace

IndexDatabaseSstWriterContext::Artifacts
IndexDatabaseSstWriterContext::Artifacts::move_to(
    std::string_view dest_dir) && {
    const fs::path dir(dest_dir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        throw IndexerError(IndexerError::Type::FILE_ERROR,
                           "Failed to create SST move destination '" +
                               std::string(dest_dir) + "': " + ec.message());
    }

    Artifacts out;
    move_one(dir, metadata_sst, out.metadata_sst);
    move_one(dir, checkpoints_sst, out.checkpoints_sst);
    move_one(dir, manifest_sst, out.manifest_sst);
    move_one(dir, chunk_bloom_sst, out.chunk_bloom_sst);
    move_one(dir, file_bloom_sst, out.file_bloom_sst);
    move_one(dir, chunk_stats_sst, out.chunk_stats_sst);
    move_one(dir, chunk_dim_stats_sst, out.chunk_dim_stats_sst);
    move_one(dir, dimensions_sst, out.dimensions_sst);
    move_one(dir, file_scalar_stats_sst, out.file_scalar_stats_sst);
    move_one(dir, file_cat_counts_sst, out.file_cat_counts_sst);
    move_one(dir, file_pid_tid_counts_sst, out.file_pid_tid_counts_sst);
    move_one(dir, file_name_counts_sst, out.file_name_counts_sst);
    move_one(dir, name_dictionary_sst, out.name_dictionary_sst);
    move_one(dir, name_file_postings_sst, out.name_file_postings_sst);
    move_one(dir, name_chunk_postings_sst, out.name_chunk_postings_sst);
    move_one(dir, hash_tables_sst, out.hash_tables_sst);
    move_one(dir, aggregation_sst, out.aggregation_sst);
    move_one(dir, system_metrics_sst, out.system_metrics_sst);
    return out;
}

IndexDatabaseSstWriterContext::IndexDatabaseSstWriterContext(
    std::string staging_dir, std::string batch_id)
    : staging_dir_(std::move(staging_dir)), batch_id_(std::move(batch_id)) {
    std::error_code ec;
    fs::create_directories(fs::path(staging_dir_) / batch_id_, ec);
    if (ec) {
        throw IndexerError(IndexerError::Type::FILE_ERROR,
                           "Failed to create SST staging dir '" + staging_dir_ +
                               "/" + batch_id_ + "': " + ec.message());
    }
}

IndexDatabaseSstWriterContext::IndexDatabaseSstWriterContext(
    IndexDatabaseSstWriterContext&&) noexcept = default;
IndexDatabaseSstWriterContext& IndexDatabaseSstWriterContext::operator=(
    IndexDatabaseSstWriterContext&&) noexcept = default;
IndexDatabaseSstWriterContext::~IndexDatabaseSstWriterContext() = default;

void IndexDatabaseSstWriterContext::insert_file_metadata(
    int file_id, std::uint64_t checkpoint_size, std::uint64_t total_lines,
    std::uint64_t total_uc_size) {
    metadata_buf_.emplace_back(
        encoding::metadata_key(file_id),
        encoding::encode_metadata_record(checkpoint_size, total_lines,
                                         total_uc_size));
}

void IndexDatabaseSstWriterContext::insert_checkpoint(
    int file_id, const IndexerCheckpoint& checkpoint) {
    checkpoints_buf_.emplace_back(
        encoding::checkpoint_key(file_id, checkpoint.uc_offset,
                                 checkpoint.checkpoint_idx),
        encoding::encode_checkpoint_value(checkpoint));
}

void IndexDatabaseSstWriterContext::insert_event_range(
    int file_id, std::uint64_t checkpoint_idx, std::string_view cat,
    std::string_view name, std::span<const std::uint32_t> line_numbers) {
    manifest_buf_.emplace_back(
        encoding::manifest_event_key(file_id, checkpoint_idx, cat, name),
        encoding::encode_event_range_value(line_numbers));
}

void IndexDatabaseSstWriterContext::insert_metadata_lines(
    int file_id, std::uint64_t checkpoint_idx, std::string_view meta_type,
    std::span<const std::uint32_t> line_numbers) {
    manifest_buf_.emplace_back(
        encoding::manifest_metadata_key(file_id, checkpoint_idx, meta_type),
        encoding::encode_metadata_value(line_numbers));
}

void IndexDatabaseSstWriterContext::insert_file_pids(
    int file_id, const std::unordered_set<std::uint64_t>& pids) {
    manifest_buf_.emplace_back(encoding::file_pids_key(file_id),
                               encoding::encode_file_pids_value(pids));
}

void IndexDatabaseSstWriterContext::insert_chunk_bloom_filter(
    int file_id, std::uint64_t checkpoint_idx, std::string_view dimension,
    std::span<const unsigned char> blob_data, std::uint64_t num_entries) {
    chunk_bloom_buf_.emplace_back(
        encoding::chunk_bloom_key(file_id, dimension, checkpoint_idx),
        encoding::encode_bloom_value(blob_data, num_entries));
}

void IndexDatabaseSstWriterContext::insert_file_bloom_filter(
    int file_id, std::string_view dimension,
    std::span<const unsigned char> blob_data, std::uint64_t num_entries) {
    file_bloom_buf_.emplace_back(
        encoding::file_bloom_key(file_id, dimension),
        encoding::encode_bloom_value(blob_data, num_entries));
}

void IndexDatabaseSstWriterContext::insert_chunk_statistics(
    int file_id, std::uint64_t checkpoint_idx, const ChunkStatistics& stats) {
    chunk_stats_buf_.emplace_back(
        encoding::chunk_stats_key(file_id, checkpoint_idx),
        encoding::encode_chunk_statistics_value(stats));
}

void IndexDatabaseSstWriterContext::insert_file_scalar_stats(
    int file_id, const ChunkStatistics& stats, std::uint64_t num_chunks) {
    file_scalar_stats_buf_.emplace_back(
        encoding::file_scalar_stats_key(file_id),
        internal::encode_file_scalar_stats_value(stats, num_chunks));
}

void IndexDatabaseSstWriterContext::insert_file_category_counts(
    int file_id, const StringViewMap<std::uint64_t>& counts) {
    file_cat_counts_buf_.emplace_back(
        encoding::file_category_counts_key(file_id),
        encoding::encode_count_map_value(counts));
}

void IndexDatabaseSstWriterContext::insert_file_pid_tid_counts(
    int file_id, const StringViewMap<std::uint64_t>& counts) {
    file_pid_tid_counts_buf_.emplace_back(
        encoding::file_pid_tid_counts_key(file_id),
        encoding::encode_count_map_value(counts));
}

void IndexDatabaseSstWriterContext::insert_file_name_counts(
    int file_id, const StringViewMap<std::uint64_t>& counts) {
    file_name_counts_buf_.emplace_back(
        encoding::file_name_counts_key(file_id),
        encoding::encode_name_summary_value(counts, /*other_count=*/0,
                                            /*unique_count=*/counts.size()));
}

void IndexDatabaseSstWriterContext::insert_index_dimension(
    int file_id, std::string_view dimension) {
    dimensions_buf_.emplace_back(
        encoding::make_dimension_key(file_id, dimension), std::string{});
}

void IndexDatabaseSstWriterContext::insert_chunk_dimension_stats(
    int file_id, std::uint64_t checkpoint_idx, const ChunkDimensionStats& stats,
    std::size_t value_counts_cap) {
    chunk_dim_stats_buf_.emplace_back(
        encoding::chunk_dim_stats_key(file_id, checkpoint_idx, stats.dimension),
        encoding::encode_chunk_dimension_stats_value(stats, value_counts_cap));
}

void IndexDatabaseSstWriterContext::insert_name_dictionary_entry(
    std::uint64_t name_id, std::string_view name) {
    name_dictionary_buf_.emplace_back(
        encoding::name_lookup_key(name),
        ::dftracer::utils::rocksdb::KeyCodec::encode_be64(name_id));
    name_dictionary_buf_.emplace_back(encoding::name_reverse_key(name_id),
                                      std::string(name));
}

void IndexDatabaseSstWriterContext::insert_name_file_posting(
    std::uint64_t name_id, int file_id) {
    name_file_postings_buf_.emplace_back(
        encoding::name_file_posting_key(name_id, file_id), std::string{});
    name_file_postings_buf_.emplace_back(
        encoding::name_file_owner_key(file_id, name_id), std::string{});
}

void IndexDatabaseSstWriterContext::insert_name_chunk_posting(
    std::uint64_t name_id, int file_id, std::uint64_t checkpoint_idx) {
    name_chunk_postings_buf_.emplace_back(
        encoding::name_chunk_posting_key(name_id, file_id, checkpoint_idx),
        std::string{});
    name_chunk_postings_buf_.emplace_back(
        encoding::name_chunk_owner_key(file_id, name_id, checkpoint_idx),
        std::string{});
}

void IndexDatabaseSstWriterContext::insert_hash_table_entry(
    std::uint8_t type, std::string_view hash, std::string_view name) {
    hash_tables_buf_.emplace_back(encoding::hash_table_forward_key(type, hash),
                                  std::string(name));
    hash_tables_buf_.emplace_back(encoding::hash_table_reverse_key(type, name),
                                  std::string(hash));
}

void IndexDatabaseSstWriterContext::insert_aggregation_merge(
    std::string_view key, std::string_view operand) {
    aggregation_buf_.emplace_back(MergeableKeyValue{
        std::string(key), std::string(operand), /*is_merge=*/true});
}

void IndexDatabaseSstWriterContext::insert_aggregation_put(
    std::string_view key, std::string_view value) {
    aggregation_buf_.emplace_back(MergeableKeyValue{
        std::string(key), std::string(value), /*is_merge=*/false});
}

void IndexDatabaseSstWriterContext::insert_system_metrics_merge(
    std::string_view key, std::string_view operand) {
    system_metrics_buf_.emplace_back(MergeableKeyValue{
        std::string(key), std::string(operand), /*is_merge=*/true});
}

IndexDatabaseSstWriterContext::Artifacts
IndexDatabaseSstWriterContext::commit() {
    Artifacts out;
    if (committed_) {
        return out;
    }
    committed_ = true;

    const auto batch_dir = fs::path(staging_dir_) / batch_id_;

    auto emit_into = [&](const char* name, std::vector<KeyValue>& buf,
                         std::optional<std::string>& slot) {
        if (buf.empty()) return;
        slot = emit_sst((batch_dir / name).string(), buf);
        buf.clear();
        buf.shrink_to_fit();
    };

    emit_into("metadata.sst", metadata_buf_, out.metadata_sst);
    emit_into("checkpoints.sst", checkpoints_buf_, out.checkpoints_sst);
    emit_into("manifest.sst", manifest_buf_, out.manifest_sst);
    emit_into("chunk_bloom.sst", chunk_bloom_buf_, out.chunk_bloom_sst);
    emit_into("file_bloom.sst", file_bloom_buf_, out.file_bloom_sst);
    emit_into("chunk_stats.sst", chunk_stats_buf_, out.chunk_stats_sst);
    emit_into("chunk_dim_stats.sst", chunk_dim_stats_buf_,
              out.chunk_dim_stats_sst);
    emit_into("dimensions.sst", dimensions_buf_, out.dimensions_sst);
    emit_into("file_scalar_stats.sst", file_scalar_stats_buf_,
              out.file_scalar_stats_sst);
    emit_into("file_cat_counts.sst", file_cat_counts_buf_,
              out.file_cat_counts_sst);
    emit_into("file_pid_tid_counts.sst", file_pid_tid_counts_buf_,
              out.file_pid_tid_counts_sst);
    emit_into("file_name_counts.sst", file_name_counts_buf_,
              out.file_name_counts_sst);
    emit_into("name_dictionary.sst", name_dictionary_buf_,
              out.name_dictionary_sst);
    emit_into("name_file_postings.sst", name_file_postings_buf_,
              out.name_file_postings_sst);
    emit_into("name_chunk_postings.sst", name_chunk_postings_buf_,
              out.name_chunk_postings_sst);
    emit_into("hash_tables.sst", hash_tables_buf_, out.hash_tables_sst);

    auto emit_mixed_into = [&](const char* name,
                               std::vector<MergeableKeyValue>& buf,
                               std::optional<std::string>& slot) {
        if (buf.empty()) return;
        slot = emit_mixed_sst((batch_dir / name).string(), buf);
        buf.clear();
        buf.shrink_to_fit();
    };
    emit_mixed_into("aggregation.sst", aggregation_buf_, out.aggregation_sst);
    emit_mixed_into("system_metrics.sst", system_metrics_buf_,
                    out.system_metrics_sst);

    return out;
}

}  // namespace dftracer::utils::utilities::indexer
