#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_INDEX_SCHEMA_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_INDEX_SCHEMA_H

#include <dftracer/utils/core/sqlite/database.h>

#include <cstdint>
#include <string>

namespace dftracer::utils::utilities::composites::dft::indexing {

/**
 * @brief Manages the .bidx SQLite database for bloom filter indices.
 *
 * Separate sidecar database from the .idx gzip checkpoint index.
 * Path convention: file.pfw.gz -> file.pfw.gz.bidx
 *
 * Schema includes:
 *   - file_info: tracks indexed files with staleness detection via hash
 *   - chunk_bloom_filters: per-checkpoint per-dimension bloom filters
 *   - file_bloom_filters: merged file-level bloom filters per dimension
 *   - chunk_statistics: per-checkpoint event statistics
 *   - index_dimensions: tracks which dimensions have been indexed
 *   - hash_resolutions: maps hash values to resolved names
 */
class BloomIndexDatabase {
   public:
    explicit BloomIndexDatabase(const std::string& bidx_path);

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
 * @brief Determine the bloom index (.bidx) path for a given data file.
 */
std::string determine_bloom_index_path(const std::string& file_path,
                                       const std::string& index_dir = "");

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_INDEX_SCHEMA_H
