#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_QUERIES_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_QUERIES_H

#include <dftracer/utils/core/sqlite/database.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteDatabase;

// --- Insert operations ---

void insert_chunk_bloom_filter(const SqliteDatabase& db, int file_info_id,
                               std::uint64_t checkpoint_idx,
                               const std::string& dimension,
                               const void* blob_data, int blob_size,
                               std::uint64_t num_entries);

void insert_file_bloom_filter(const SqliteDatabase& db, int file_info_id,
                              const std::string& dimension,
                              const void* blob_data, int blob_size,
                              std::uint64_t num_entries);

void insert_chunk_statistics(const SqliteDatabase& db, int file_info_id,
                             std::uint64_t checkpoint_idx,
                             const ChunkStatistics& stats);

void insert_index_dimension(const SqliteDatabase& db, int file_info_id,
                            const std::string& dimension);

void insert_hash_resolution(const SqliteDatabase& db, int file_info_id,
                            const std::string& dimension,
                            const std::string& hash_value,
                            const std::string& resolved_value);

// --- Query operations ---

struct ChunkBloomResult {
    std::uint64_t checkpoint_idx;
    std::vector<unsigned char> bloom_data;
    std::uint64_t num_entries;
};

std::vector<ChunkBloomResult> query_chunk_bloom_filters(
    const SqliteDatabase& db, int file_info_id, const std::string& dimension);

struct FileBloomResult {
    std::vector<unsigned char> bloom_data;
    std::uint64_t num_entries;
};

std::optional<FileBloomResult> query_file_bloom_filter(
    const SqliteDatabase& db, int file_info_id, const std::string& dimension);

std::vector<std::string> query_index_dimensions(const SqliteDatabase& db,
                                                int file_info_id);

bool has_index_dimension(const SqliteDatabase& db, int file_info_id,
                         const std::string& dimension);

struct ChunkStatisticsResult {
    std::uint64_t checkpoint_idx;
    ChunkStatistics stats;
};

std::vector<ChunkStatisticsResult> query_chunk_statistics(
    const SqliteDatabase& db, int file_info_id);

std::vector<std::string> query_hash_by_resolved(
    const SqliteDatabase& db, const std::string& dimension,
    const std::string& resolved_value);

std::optional<std::string> query_resolved_by_hash(
    const SqliteDatabase& db, const std::string& dimension,
    const std::string& hash_value);

// --- Delete operations ---

void delete_chunk_bloom_filters(const SqliteDatabase& db, int file_info_id,
                                const std::string& dimension);

void delete_file_bloom_filter(const SqliteDatabase& db, int file_info_id,
                              const std::string& dimension);

void delete_chunk_statistics(const SqliteDatabase& db, int file_info_id);

void delete_hash_resolutions(const SqliteDatabase& db, int file_info_id);

}  // namespace dftracer::utils::utilities::composites::dft::indexing::queries

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_QUERIES_H
