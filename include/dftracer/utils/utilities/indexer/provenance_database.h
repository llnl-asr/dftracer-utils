#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_PROVENANCE_DATABASE_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_PROVENANCE_DATABASE_H

#include <dftracer/utils/core/sqlite/database.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::indexer {

/**
 * @brief Manages the .pidx SQLite database for provenance indices.
 *
 * Sidecar database that records the full reorganization provenance of
 * an output file: which source files contributed, which checkpoints,
 * and which line ranges map to which output lines.
 * Path convention: file.pfw.gz -> file.pfw.gz.pidx
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

    explicit ProvenanceDatabase(const std::string& pidx_path);

    ProvenanceDatabase(const ProvenanceDatabase&) = delete;
    ProvenanceDatabase& operator=(const ProvenanceDatabase&) = delete;

    ProvenanceDatabase(ProvenanceDatabase&&) noexcept = default;
    ProvenanceDatabase& operator=(ProvenanceDatabase&&) noexcept = default;

    void init_schema();

    int get_or_create_file_info(const std::string& path,
                                std::uint64_t file_hash);

    int get_file_info_id(const std::string& path) const;

    dftracer::utils::sqlite::SqliteDatabase& db() { return db_; }
    const dftracer::utils::sqlite::SqliteDatabase& db() const { return db_; }

    void begin_transaction();
    void commit_transaction();

    // -----------------------------------------------------------------------
    // Provenance insert operations
    // -----------------------------------------------------------------------

    void insert_info(std::string_view key, std::string_view value);

    void insert_source(int file_info_id, int source_idx, std::string_view path,
                       int num_checkpoints, std::string_view event_hash = "");

    void insert_group(std::string_view name, std::string_view predicate);

    void insert_segment(int source_idx, int source_checkpoint,
                        int output_line_start, int output_line_end,
                        int event_count);

    // -----------------------------------------------------------------------
    // Provenance query operations
    // -----------------------------------------------------------------------

    std::vector<ProvenanceSource> query_sources(int file_info_id) const;

    std::vector<ProvenanceSegment> query_segments(int source_idx) const;

    std::vector<ProvenanceSegment> query_all_segments() const;

    std::string query_info(std::string_view key) const;

    std::string query_group_name() const;

    std::string query_group_predicate() const;

   private:
    dftracer::utils::sqlite::SqliteDatabase db_;
};

/**
 * @brief Determine the provenance index (.pidx) path for a given data file.
 */
std::string determine_provenance_index_path(const std::string& data_path,
                                            const std::string& index_dir = "");

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_PROVENANCE_DATABASE_H
