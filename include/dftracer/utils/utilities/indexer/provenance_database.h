#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_PROVENANCE_DATABASE_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_PROVENANCE_DATABASE_H

#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/rocksdb/db_manager.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::indexer {

/**
 * @brief Manages provenance data in the shared `.dftindex` RocksDB store.
 *
 * Shared index data that records the full reorganization provenance of
 * an output file: which source files contributed, which checkpoints,
 * and which line ranges map to which output lines.
 *
 * Schema:
 *   - file_info: output file identity (path + hash)
 *   - provenance_info: key/value metadata (tool version, timestamp, etc.)
 *   - provenance_sources: source files that contributed to this output
 *   - provenance_group: named predicate groups used during reorganization
 *   - provenance_segments: per-checkpoint line range mappings
 */
class ProvenanceDatabase {
   public:
    // Re-export result types
    using ProvenanceSource =
        composites::dft::indexing::queries::ProvenanceSource;
    using ProvenanceSegment =
        composites::dft::indexing::queries::ProvenanceSegment;

    explicit ProvenanceDatabase(
        const std::string& provenance_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode open_mode =
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadWrite);

    ProvenanceDatabase(const ProvenanceDatabase&) = delete;
    ProvenanceDatabase& operator=(const ProvenanceDatabase&) = delete;

    ProvenanceDatabase(ProvenanceDatabase&&) noexcept = default;
    ProvenanceDatabase& operator=(ProvenanceDatabase&&) noexcept = default;

    void init_schema();

    int get_or_create_file_info(const std::string& path,
                                std::uint64_t file_hash);

    int get_file_info_id(const std::string& path) const;

    void begin_transaction();
    void commit_transaction();
    void rollback_transaction() noexcept;

    // -----------------------------------------------------------------------
    // Provenance insert operations
    // -----------------------------------------------------------------------

    void insert_info(int file_info_id, std::string_view key,
                     std::string_view value);

    void insert_source(int file_info_id, int source_idx, std::string_view path,
                       int num_checkpoints, std::string_view event_hash = "");

    void insert_group(int file_info_id, std::string_view name,
                      std::string_view predicate);

    void insert_segment(int file_info_id, int source_idx, int source_checkpoint,
                        int output_line_start, int output_line_end,
                        int event_count);

    // -----------------------------------------------------------------------
    // Provenance query operations
    // -----------------------------------------------------------------------

    std::vector<ProvenanceSource> query_sources(int file_info_id) const;

    std::vector<ProvenanceSegment> query_segments(int file_info_id,
                                                  int source_idx) const;

    std::vector<ProvenanceSegment> query_all_segments(int file_info_id) const;

    std::string query_info(int file_info_id, std::string_view key) const;

    std::string query_group_name(int file_info_id) const;

    std::string query_group_predicate(int file_info_id) const;

   private:
    std::string db_path_;
    dftracer::utils::rocksdb::RocksDatabase::OpenMode open_mode_;
    std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> db_;
    std::unique_ptr<dftracer::utils::rocksdb::RocksDatabase::Batch> txn_batch_;
};

/**
 * @brief Determine the shared `.dftindex` provenance root for a data file.
 */
std::string determine_provenance_index_path(const std::string& data_path,
                                            const std::string& index_dir = "");

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_PROVENANCE_DATABASE_H
