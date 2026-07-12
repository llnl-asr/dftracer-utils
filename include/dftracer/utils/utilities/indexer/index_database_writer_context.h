#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_WRITER_CONTEXT_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_WRITER_CONTEXT_H

#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/rocksdb/db_manager.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/index_batch_sink.h>
#include <dftracer/utils/utilities/indexer/index_file_entry_capability.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::utilities::indexer {

class IndexDatabase;

class IndexDatabaseWriterContext : public IndexBatchSink {
   public:
    using IndexBatchSink::ChunkDimensionStats;
    using IndexBatchSink::ChunkStatistics;
    using IndexBatchSink::IndexerCheckpoint;
    using IndexBatchSink::insert_event_range;
    using IndexBatchSink::insert_metadata_lines;
    struct TarFileRecord {
        std::string file_name;
        std::uint64_t file_size = 0;
        std::uint64_t file_mtime = 0;
        char typeflag = '\0';
        std::uint64_t data_offset = 0;
        std::uint64_t uncompressed_offset = 0;
    };

    IndexDatabaseWriterContext(IndexDatabaseWriterContext&&) noexcept;
    IndexDatabaseWriterContext& operator=(
        IndexDatabaseWriterContext&&) noexcept;
    IndexDatabaseWriterContext(const IndexDatabaseWriterContext&) = delete;
    IndexDatabaseWriterContext& operator=(const IndexDatabaseWriterContext&) =
        delete;
    ~IndexDatabaseWriterContext() override;

    void commit();

    // Read-through queries (needed by visitors during write)
    bool has_file_scalar_stats(int file_id) const;

    // Schema initialisation
    void init_schema();

    // Registry/capability writes
    int get_or_create_file_info(
        std::string_view path, std::uint64_t file_hash,
        IndexFileEntryCapability caps = IndexFileEntryCapability::NONE,
        std::uint64_t file_mtime = 0, std::uint64_t file_size = 0);
    void set_file_capabilities(int file_id, IndexFileEntryCapability caps);
    void set_file_capabilities_by_path(std::string_view logical_path,
                                       IndexFileEntryCapability caps);
    void add_file_capability(int file_id, IndexFileEntryCapability cap);

    // Metadata
    void insert_file_metadata(int file_id, std::uint64_t checkpoint_size,
                              std::uint64_t total_lines,
                              std::uint64_t total_uc_size) override;

    // Bloom inserts
    void insert_chunk_bloom_filter(int file_id, std::uint64_t checkpoint_idx,
                                   std::string_view dimension,
                                   std::span<const unsigned char> blob_data,
                                   std::uint64_t num_entries) override;

    void insert_chunk_bloom_filter(int file_id, std::uint64_t checkpoint_idx,
                                   std::string_view dimension,
                                   const void* blob_data, int blob_size,
                                   std::uint64_t num_entries);

    void insert_file_bloom_filter(int file_id, std::string_view dimension,
                                  std::span<const unsigned char> blob_data,
                                  std::uint64_t num_entries) override;

    void insert_file_bloom_filter(int file_id, std::string_view dimension,
                                  const void* blob_data, int blob_size,
                                  std::uint64_t num_entries);

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
    std::uint64_t get_or_create_name_id(std::string_view name);
    void insert_name_dictionary_entry(std::uint64_t name_id,
                                      std::string_view name) override;
    void insert_name_file_posting(std::uint64_t name_id, int file_id) override;
    void insert_name_chunk_posting(std::uint64_t name_id, int file_id,
                                   std::uint64_t checkpoint_idx) override;
    void refresh_root_summaries_after_file_write(
        int file_id, const ChunkStatistics& stats, std::uint64_t num_chunks,
        bool had_existing_file_summary, std::uint64_t file_lines = 0,
        std::uint64_t file_uncompressed_bytes = 0);
    void rebuild_root_summaries();
    void insert_checkpoint(int file_id,
                           const IndexerCheckpoint& checkpoint) override;

    void insert_index_dimension(int file_id,
                                std::string_view dimension) override;

    /// Insert a hash table entry with bidirectional storage.
    /// Forward: [type][hash] -> name  (for output resolution)
    /// Reverse: [type+4][name] -> hash  (for query DSL)
    /// Type: 0=FILE, 1=HOST, 2=STRING, 3=PROC
    void insert_hash_table_entry(std::uint8_t type, std::string_view hash,
                                 std::string_view name) override;

    // Aggregation / system-metrics CF writes.
    void insert_aggregation_merge(std::string_view key,
                                  std::string_view operand) override;

    void insert_aggregation_put(std::string_view key,
                                std::string_view value) override;

    void insert_system_metrics_merge(std::string_view key,
                                     std::string_view operand) override;

    void insert_chunk_dimension_stats(
        int file_id, std::uint64_t checkpoint_idx,
        const ChunkDimensionStats& stats,
        std::size_t value_counts_cap = 4096) override;
    void insert_tar_archive_metadata(int file_id, std::string_view archive_name,
                                     std::uint64_t checkpoint_size,
                                     std::uint64_t total_lines,
                                     std::uint64_t total_uc_size,
                                     std::uint64_t total_files);
    void insert_tar_file(int file_id, const TarFileRecord& record);

    // Deletes
    void delete_chunk_bloom_filters(int file_id, std::string_view dimension);
    void delete_file_bloom_filter(int file_id, std::string_view dimension);
    void delete_chunk_statistics(int file_id);
    void delete_chunk_dimension_stats(int file_id);
    void delete_file_contents(int file_id);
    void delete_event_ranges(int file_id);
    void delete_metadata_lines(int file_id);

    // Manifest inserts
    void insert_event_range(
        int file_id, std::uint64_t checkpoint_idx, std::string_view cat,
        std::string_view name,
        std::span<const std::uint32_t> line_numbers) override;

    void insert_metadata_lines(
        int file_id, std::uint64_t checkpoint_idx, std::string_view meta_type,
        std::span<const std::uint32_t> line_numbers) override;

    /// Insert the set of PIDs observed in a file (for distributed aggregation)
    void insert_file_pids(
        int file_id, const std::unordered_set<std::uint64_t>& pids) override;

   private:
    friend class IndexDatabase;
    explicit IndexDatabaseWriterContext(
        std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> db);

    std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> db_;
    dftracer::utils::rocksdb::RocksDatabase::Batch batch_;
    bool committed_ = false;
    std::int64_t cached_next_file_id_ = -1;
};

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_WRITER_CONTEXT_H
