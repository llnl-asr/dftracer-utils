#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_H

#include <dftracer/utils/core/sqlite/database.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::indexer {

/**
 * @brief Unified .idx SQLite database combining checkpoint, bloom filter,
 *        and manifest data in a single sidecar file.
 *
 * Schema is additive: call init_base_schema() always, then
 * init_bloom_schema() and/or init_manifest_schema() as needed.
 *
 * All query/insert/delete operations are exposed as methods so callers
 * never need to use the queries:: namespace directly.
 */
class IndexDatabase {
   public:
    // Re-export result types so callers don't need query headers
    using ChunkBloomResult =
        composites::dft::indexing::queries::ChunkBloomResult;
    using FileBloomResult = composites::dft::indexing::queries::FileBloomResult;
    using ChunkStatisticsResult =
        composites::dft::indexing::queries::ChunkStatisticsResult;
    using TimeBounds = composites::dft::indexing::queries::TimeBounds;
    using EventRangeResult =
        composites::dft::indexing::queries::EventRangeResult;
    using MetadataLinesResult =
        composites::dft::indexing::queries::MetadataLinesResult;
    using ChunkStatistics = composites::dft::indexing::ChunkStatistics;
    using ChunkDimensionStats = composites::dft::indexing::ChunkDimensionStats;
    using ChunkDimensionStatsResult =
        composites::dft::indexing::ChunkDimensionStatsResult;

    explicit IndexDatabase(const std::string& idx_path);

    IndexDatabase(const IndexDatabase&) = delete;
    IndexDatabase& operator=(const IndexDatabase&) = delete;

    IndexDatabase(IndexDatabase&&) noexcept = default;
    IndexDatabase& operator=(IndexDatabase&&) noexcept = default;

    ~IndexDatabase() = default;

    // Schema initialisation — idempotent (CREATE TABLE IF NOT EXISTS)
    void init_base_schema();
    void init_bloom_schema();
    void init_manifest_schema();

    // Query helpers
    bool has_bloom_data(int file_id) const;
    bool has_manifest_data(int file_id) const;

    int get_or_create_file_info(std::string_view path, std::uint64_t file_hash);
    int get_file_info_id(std::string_view path) const;

    // Convenience: resolve file path to file_id (handles logical path)
    int find_file(std::string_view file_path) const;

    // Metadata queries
    std::uint64_t get_num_lines(int file_id) const;
    std::uint64_t get_max_bytes(int file_id) const;

    // Returns exact event count from chunk_statistics if bloom was built,
    // otherwise falls back to num_lines (approximate).
    std::uint64_t get_total_events(int file_id) const;

    void begin_transaction();
    void commit_transaction();

    sqlite3* db() const { return db_.get(); }
    dftracer::utils::sqlite::SqliteDatabase& sql_db() { return db_; }
    const dftracer::utils::sqlite::SqliteDatabase& sql_db() const {
        return db_;
    }

    // -----------------------------------------------------------------------
    // Bloom insert operations
    // -----------------------------------------------------------------------

    void insert_chunk_bloom_filter(int file_id, std::uint64_t checkpoint_idx,
                                   std::string_view dimension,
                                   std::span<const unsigned char> blob_data,
                                   std::uint64_t num_entries);

    void insert_chunk_bloom_filter(int file_id, std::uint64_t checkpoint_idx,
                                   std::string_view dimension,
                                   const void* blob_data, int blob_size,
                                   std::uint64_t num_entries);

    void insert_file_bloom_filter(int file_id, std::string_view dimension,
                                  std::span<const unsigned char> blob_data,
                                  std::uint64_t num_entries);

    void insert_file_bloom_filter(int file_id, std::string_view dimension,
                                  const void* blob_data, int blob_size,
                                  std::uint64_t num_entries);

    void insert_chunk_statistics(int file_id, std::uint64_t checkpoint_idx,
                                 const ChunkStatistics& stats);

    void insert_index_dimension(int file_id, std::string_view dimension);

    void insert_hash_resolution(int file_id, std::string_view dimension,
                                std::string_view hash_value,
                                std::string_view resolved_value);

    void insert_chunk_dimension_stats(int file_id, std::uint64_t checkpoint_idx,
                                      const ChunkDimensionStats& stats,
                                      std::size_t value_counts_cap = 4096);

    // -----------------------------------------------------------------------
    // Bloom query operations
    // -----------------------------------------------------------------------

    std::vector<ChunkBloomResult> query_chunk_bloom_filters(
        int file_id, std::string_view dimension) const;

    std::unordered_map<std::string, std::vector<ChunkBloomResult>>
    query_chunk_bloom_filters_batch(
        int file_id, const std::vector<std::string>& dimensions) const;

    std::optional<FileBloomResult> query_file_bloom_filter(
        int file_id, std::string_view dimension) const;

    std::unordered_map<std::string, FileBloomResult>
    query_file_bloom_filters_batch(
        int file_id, const std::vector<std::string>& dimensions) const;

    std::vector<std::string> query_index_dimensions(int file_id) const;

    bool has_index_dimension(int file_id, std::string_view dimension) const;

    std::vector<ChunkStatisticsResult> query_chunk_statistics(
        int file_id) const;

    TimeBounds query_time_bounds(int file_id) const;

    std::vector<ChunkDimensionStatsResult> query_chunk_dimension_stats(
        int file_id) const;

    std::vector<ChunkDimensionStatsResult>
    query_chunk_dimension_stats_for_dimension(int file_id,
                                              std::string_view dimension) const;

    // Global queries (search across all files)
    std::optional<std::string> query_resolved_by_hash(
        std::string_view dimension, std::string_view hash_value) const;

    std::vector<std::string> query_hash_by_resolved(
        std::string_view dimension, std::string_view resolved_value) const;

    // -----------------------------------------------------------------------
    // Bloom delete operations
    // -----------------------------------------------------------------------

    void delete_chunk_bloom_filters(int file_id, std::string_view dimension);
    void delete_file_bloom_filter(int file_id, std::string_view dimension);
    void delete_chunk_statistics(int file_id);
    void delete_chunk_dimension_stats(int file_id);
    void delete_hash_resolutions(int file_id);

    // -----------------------------------------------------------------------
    // Manifest insert operations
    // -----------------------------------------------------------------------

    void insert_event_range(int file_id, std::uint64_t checkpoint_idx,
                            std::string_view cat, std::string_view name,
                            std::span<const std::uint32_t> line_numbers);

    void insert_event_range(int file_id, std::uint64_t checkpoint_idx,
                            std::string_view cat, std::string_view name,
                            const std::vector<std::uint32_t>& line_numbers);

    void insert_metadata_lines(int file_id, std::uint64_t checkpoint_idx,
                               std::string_view meta_type,
                               std::span<const std::uint32_t> line_numbers);

    void insert_metadata_lines(int file_id, std::uint64_t checkpoint_idx,
                               std::string_view meta_type,
                               const std::vector<std::uint32_t>& line_numbers);

    // -----------------------------------------------------------------------
    // Manifest query operations
    // -----------------------------------------------------------------------

    std::vector<EventRangeResult> query_event_ranges(int file_id) const;

    std::vector<EventRangeResult> query_event_ranges_for_checkpoint(
        int file_id, std::uint64_t checkpoint_idx) const;

    std::vector<MetadataLinesResult> query_metadata_lines(int file_id) const;

    std::vector<MetadataLinesResult> query_metadata_lines_for_checkpoint(
        int file_id, std::uint64_t checkpoint_idx) const;

    // -----------------------------------------------------------------------
    // Manifest delete operations
    // -----------------------------------------------------------------------

    void delete_event_ranges(int file_id);
    void delete_metadata_lines(int file_id);

   private:
    dftracer::utils::sqlite::SqliteDatabase db_;
};

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_DATABASE_H
