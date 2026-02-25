#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_INDEX_SCHEMA_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_INDEX_SCHEMA_H

#include <dftracer/utils/core/sqlite/database.h>

#include <cstdint>
#include <string>

namespace dftracer::utils::utilities::composites::dft::indexing {

/**
 * @brief Manages the .midx SQLite database for manifest indices.
 *
 * Separate sidecar database that stores per-checkpoint, per-(cat, name)
 * line number groups for lossless trace reorganization.
 * Path convention: file.pfw.gz -> file.pfw.gz.midx
 *
 * Schema includes:
 *   - file_info: tracks indexed files with staleness detection via hash
 *   - checkpoint_event_ranges: per-checkpoint per-(cat, name) line numbers
 *   - checkpoint_metadata_lines: per-checkpoint metadata line numbers
 */
class ManifestIndexDatabase {
   public:
    explicit ManifestIndexDatabase(const std::string& midx_path);

    void init_schema();

    int get_or_create_file_info(const std::string& file_path,
                                std::uint64_t file_hash);

    int get_file_info_id(const std::string& file_path) const;

    dftracer::utils::sqlite::SqliteDatabase& db() { return db_; }
    const dftracer::utils::sqlite::SqliteDatabase& db() const { return db_; }

    void begin_transaction();
    void commit_transaction();

   private:
    dftracer::utils::sqlite::SqliteDatabase db_;
};

/**
 * @brief Determine the manifest index (.midx) path for a given data file.
 */
std::string determine_manifest_index_path(const std::string& file_path,
                                          const std::string& index_dir = "");

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_INDEX_SCHEMA_H
