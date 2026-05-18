#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_SST_WRITER_CONTEXT_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_SST_WRITER_CONTEXT_H

#include <dftracer/utils/utilities/indexer/index_batch_sink.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::indexer {

/// Per-batch SST emitter that implements `IndexBatchSink` by buffering
/// (key, value) pairs in memory per column family and flushing them to sorted
/// SST files on `commit()`.
///
/// Usage is identical to `IndexDatabaseWriterContext`: construct one per
/// batch, call the `insert_*` methods, then `commit()`. The returned
/// `Artifacts` hold the paths of the SST files produced, which a coordinator
/// later ingests via `IndexDatabase::bulk_ingest()`.
///
/// Process-safe: holds no RocksDB handle. Many contexts run concurrently
/// across threads or processes, provided each is given a disjoint file_id
/// range so SST key prefixes do not overlap.
class IndexDatabaseSstWriterContext : public IndexBatchSink {
   public:
    struct Artifacts {
        std::optional<std::string> metadata_sst;
        std::optional<std::string> checkpoints_sst;
        std::optional<std::string> manifest_sst;
        std::optional<std::string> chunk_bloom_sst;
        std::optional<std::string> file_bloom_sst;
        std::optional<std::string> chunk_stats_sst;
        std::optional<std::string> chunk_dim_stats_sst;
        std::optional<std::string> dimensions_sst;
        std::optional<std::string> file_scalar_stats_sst;
        std::optional<std::string> file_cat_counts_sst;
        std::optional<std::string> file_pid_tid_counts_sst;
        std::optional<std::string> file_name_counts_sst;
        std::optional<std::string> name_dictionary_sst;
        std::optional<std::string> name_file_postings_sst;
        std::optional<std::string> name_chunk_postings_sst;
        std::optional<std::string> hash_tables_sst;
        std::optional<std::string> aggregation_sst;
        std::optional<std::string> system_metrics_sst;

        bool empty() const noexcept {
            return !metadata_sst.has_value() && !checkpoints_sst.has_value() &&
                   !manifest_sst.has_value() && !chunk_bloom_sst.has_value() &&
                   !file_bloom_sst.has_value() &&
                   !chunk_stats_sst.has_value() &&
                   !chunk_dim_stats_sst.has_value() &&
                   !dimensions_sst.has_value() &&
                   !file_scalar_stats_sst.has_value() &&
                   !file_cat_counts_sst.has_value() &&
                   !file_pid_tid_counts_sst.has_value() &&
                   !file_name_counts_sst.has_value() &&
                   !name_dictionary_sst.has_value() &&
                   !name_file_postings_sst.has_value() &&
                   !name_chunk_postings_sst.has_value() &&
                   !hash_tables_sst.has_value() &&
                   !aggregation_sst.has_value() &&
                   !system_metrics_sst.has_value();
        }

        /// Move every populated SST file to `dest_dir` (created if missing)
        /// and return a new Artifacts whose paths point at the new location.
        /// Uses `fs::rename` when src and dst resolve to the same filesystem
        /// (O(1), atomic) and falls back to copy + unlink across filesystems.
        /// Intended for the node-local -> shared FS handoff in the
        /// distributed indexer. Rvalue-qualified: the original Artifacts is
        /// left empty.
        Artifacts move_to(std::string_view dest_dir) &&;
    };

    /// Build SSTs into a unique subdirectory under `staging_dir`. `batch_id`
    /// must be unique across concurrent writers pointing at the same staging
    /// root so paths do not collide.
    IndexDatabaseSstWriterContext(std::string staging_dir,
                                  std::string batch_id);

    IndexDatabaseSstWriterContext(const IndexDatabaseSstWriterContext&) =
        delete;
    IndexDatabaseSstWriterContext& operator=(
        const IndexDatabaseSstWriterContext&) = delete;

    IndexDatabaseSstWriterContext(IndexDatabaseSstWriterContext&&) noexcept;
    IndexDatabaseSstWriterContext& operator=(
        IndexDatabaseSstWriterContext&&) noexcept;

    ~IndexDatabaseSstWriterContext() override;

    using IndexBatchSink::insert_event_range;
    using IndexBatchSink::insert_metadata_lines;

    void insert_file_metadata(int file_id, std::uint64_t checkpoint_size,
                              std::uint64_t total_lines,
                              std::uint64_t total_uc_size) override;

    void insert_checkpoint(int file_id,
                           const IndexerCheckpoint& checkpoint) override;

    void insert_event_range(
        int file_id, std::uint64_t checkpoint_idx, std::string_view cat,
        std::string_view name,
        std::span<const std::uint32_t> line_numbers) override;

    void insert_metadata_lines(
        int file_id, std::uint64_t checkpoint_idx, std::string_view meta_type,
        std::span<const std::uint32_t> line_numbers) override;

    void insert_file_pids(
        int file_id, const std::unordered_set<std::uint64_t>& pids) override;

    void insert_chunk_bloom_filter(int file_id, std::uint64_t checkpoint_idx,
                                   std::string_view dimension,
                                   std::span<const unsigned char> blob_data,
                                   std::uint64_t num_entries) override;

    void insert_file_bloom_filter(int file_id, std::string_view dimension,
                                  std::span<const unsigned char> blob_data,
                                  std::uint64_t num_entries) override;

    void insert_chunk_statistics(int file_id, std::uint64_t checkpoint_idx,
                                 const ChunkStatistics& stats) override;

    void insert_file_scalar_stats(int file_id, const ChunkStatistics& stats,
                                  std::uint64_t num_chunks) override;

    void insert_file_category_counts(
        int file_id, const StringViewMap<std::uint64_t>& counts) override;

    void insert_file_pid_tid_counts(
        int file_id, const StringViewMap<std::uint64_t>& counts) override;

    void insert_file_name_counts(
        int file_id, const StringViewMap<std::uint64_t>& counts) override;

    void insert_index_dimension(int file_id,
                                std::string_view dimension) override;

    void insert_chunk_dimension_stats(
        int file_id, std::uint64_t checkpoint_idx,
        const ChunkDimensionStats& stats,
        std::size_t value_counts_cap = 4096) override;

    void insert_name_dictionary_entry(std::uint64_t name_id,
                                      std::string_view name) override;

    void insert_name_file_posting(std::uint64_t name_id, int file_id) override;

    void insert_name_chunk_posting(std::uint64_t name_id, int file_id,
                                   std::uint64_t checkpoint_idx) override;

    void insert_hash_table_entry(std::uint8_t type, std::string_view hash,
                                 std::string_view name) override;

    void insert_aggregation_merge(std::string_view key,
                                  std::string_view operand) override;

    void insert_aggregation_put(std::string_view key,
                                std::string_view value) override;

    void insert_system_metrics_merge(std::string_view key,
                                     std::string_view operand) override;

    /// Aggregation / system_metrics buffers hold mixed Put+Merge entries
    /// in one CF. `is_merge` distinguishes them at emit time so the SST
    /// records the right operation kind (rocksdb supports mixed-op SSTs).
    struct MergeableKeyValue {
        std::string key;
        std::string value;
        bool is_merge = true;
    };

    /// Sort buffers, emit one SST per non-empty column family, return the
    /// resulting paths. Calling twice or after a move is a no-op.
    Artifacts commit();

   private:
    using KeyValue = std::pair<std::string, std::string>;

    std::string staging_dir_;
    std::string batch_id_;
    bool committed_ = false;

    std::vector<KeyValue> metadata_buf_;
    std::vector<KeyValue> checkpoints_buf_;
    std::vector<KeyValue> manifest_buf_;
    std::vector<KeyValue> chunk_bloom_buf_;
    std::vector<KeyValue> file_bloom_buf_;
    std::vector<KeyValue> chunk_stats_buf_;
    std::vector<KeyValue> chunk_dim_stats_buf_;
    std::vector<KeyValue> dimensions_buf_;
    std::vector<KeyValue> file_scalar_stats_buf_;
    std::vector<KeyValue> file_cat_counts_buf_;
    std::vector<KeyValue> file_pid_tid_counts_buf_;
    std::vector<KeyValue> file_name_counts_buf_;
    std::vector<KeyValue> name_dictionary_buf_;
    std::vector<KeyValue> name_file_postings_buf_;
    std::vector<KeyValue> name_chunk_postings_buf_;
    std::vector<KeyValue> hash_tables_buf_;
    std::vector<MergeableKeyValue> aggregation_buf_;
    std::vector<MergeableKeyValue> system_metrics_buf_;
};

/// Thread-safe collector for SST artifacts produced by many concurrent
/// `IndexDatabaseSstWriterContext` instances. The coordinator hands the
/// populated registry to `IndexDatabase::bulk_ingest()`.
class SstArtifactRegistry {
   public:
    void append(IndexDatabaseSstWriterContext::Artifacts artifacts) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto move_into = [](std::vector<std::string>& dst,
                            std::optional<std::string>& src) {
            if (src) dst.push_back(std::move(*src));
        };
        move_into(metadata_, artifacts.metadata_sst);
        move_into(checkpoints_, artifacts.checkpoints_sst);
        move_into(manifest_, artifacts.manifest_sst);
        move_into(chunk_bloom_, artifacts.chunk_bloom_sst);
        move_into(file_bloom_, artifacts.file_bloom_sst);
        move_into(chunk_stats_, artifacts.chunk_stats_sst);
        move_into(chunk_dim_stats_, artifacts.chunk_dim_stats_sst);
        move_into(dimensions_, artifacts.dimensions_sst);
        move_into(file_scalar_stats_, artifacts.file_scalar_stats_sst);
        move_into(file_cat_counts_, artifacts.file_cat_counts_sst);
        move_into(file_pid_tid_counts_, artifacts.file_pid_tid_counts_sst);
        move_into(file_name_counts_, artifacts.file_name_counts_sst);
        move_into(name_dictionary_, artifacts.name_dictionary_sst);
        move_into(name_file_postings_, artifacts.name_file_postings_sst);
        move_into(name_chunk_postings_, artifacts.name_chunk_postings_sst);
        move_into(hash_tables_, artifacts.hash_tables_sst);
        move_into(aggregation_, artifacts.aggregation_sst);
        move_into(system_metrics_, artifacts.system_metrics_sst);
    }

    const std::vector<std::string>& metadata() const { return metadata_; }
    const std::vector<std::string>& checkpoints() const { return checkpoints_; }
    const std::vector<std::string>& manifest() const { return manifest_; }
    const std::vector<std::string>& chunk_bloom() const { return chunk_bloom_; }
    const std::vector<std::string>& file_bloom() const { return file_bloom_; }
    const std::vector<std::string>& chunk_stats() const { return chunk_stats_; }
    const std::vector<std::string>& chunk_dim_stats() const {
        return chunk_dim_stats_;
    }
    const std::vector<std::string>& dimensions() const { return dimensions_; }
    const std::vector<std::string>& file_scalar_stats() const {
        return file_scalar_stats_;
    }
    const std::vector<std::string>& file_cat_counts() const {
        return file_cat_counts_;
    }
    const std::vector<std::string>& file_pid_tid_counts() const {
        return file_pid_tid_counts_;
    }
    const std::vector<std::string>& file_name_counts() const {
        return file_name_counts_;
    }
    const std::vector<std::string>& name_dictionary() const {
        return name_dictionary_;
    }
    const std::vector<std::string>& name_file_postings() const {
        return name_file_postings_;
    }
    const std::vector<std::string>& name_chunk_postings() const {
        return name_chunk_postings_;
    }
    const std::vector<std::string>& hash_tables() const { return hash_tables_; }
    const std::vector<std::string>& aggregation() const { return aggregation_; }
    const std::vector<std::string>& system_metrics() const {
        return system_metrics_;
    }

   private:
    std::mutex mutex_;
    std::vector<std::string> metadata_;
    std::vector<std::string> checkpoints_;
    std::vector<std::string> manifest_;
    std::vector<std::string> chunk_bloom_;
    std::vector<std::string> file_bloom_;
    std::vector<std::string> chunk_stats_;
    std::vector<std::string> chunk_dim_stats_;
    std::vector<std::string> dimensions_;
    std::vector<std::string> file_scalar_stats_;
    std::vector<std::string> file_cat_counts_;
    std::vector<std::string> file_pid_tid_counts_;
    std::vector<std::string> file_name_counts_;
    std::vector<std::string> name_dictionary_;
    std::vector<std::string> name_file_postings_;
    std::vector<std::string> name_chunk_postings_;
    std::vector<std::string> hash_tables_;
    std::vector<std::string> aggregation_;
    std::vector<std::string> system_metrics_;
};

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_SST_WRITER_CONTEXT_H
