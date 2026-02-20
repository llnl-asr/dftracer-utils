#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_QUERIES_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_QUERIES_H

#include <dftracer/utils/utilities/indexer/internal/sqlite/database.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using indexer::internal::SqliteDatabase;

// --- Packed line numbers helpers ---

std::vector<unsigned char> pack_line_numbers(
    const std::vector<std::uint32_t>& lines);

std::vector<std::uint32_t> unpack_line_numbers(const unsigned char* data,
                                               std::size_t size);

// --- Insert operations ---

void insert_event_range(const SqliteDatabase& db, int file_info_id,
                        std::uint64_t checkpoint_idx, const std::string& cat,
                        const std::string& name,
                        const std::vector<std::uint32_t>& line_numbers);

void insert_metadata_lines(const SqliteDatabase& db, int file_info_id,
                           std::uint64_t checkpoint_idx,
                           const std::string& meta_type,
                           const std::vector<std::uint32_t>& line_numbers);

// --- Query operations ---

struct EventRangeResult {
    std::uint64_t checkpoint_idx;
    std::string cat;
    std::string name;
    std::vector<std::uint32_t> line_numbers;
    std::uint64_t event_count;
};

std::vector<EventRangeResult> query_event_ranges(const SqliteDatabase& db,
                                                 int file_info_id);

std::vector<EventRangeResult> query_event_ranges_for_checkpoint(
    const SqliteDatabase& db, int file_info_id, std::uint64_t checkpoint_idx);

struct MetadataLinesResult {
    std::uint64_t checkpoint_idx;
    std::string meta_type;
    std::vector<std::uint32_t> line_numbers;
};

std::vector<MetadataLinesResult> query_metadata_lines(const SqliteDatabase& db,
                                                      int file_info_id);

std::vector<MetadataLinesResult> query_metadata_lines_for_checkpoint(
    const SqliteDatabase& db, int file_info_id, std::uint64_t checkpoint_idx);

// --- Delete operations ---

void delete_event_ranges(const SqliteDatabase& db, int file_info_id);

void delete_metadata_lines(const SqliteDatabase& db, int file_info_id);

}  // namespace
   // dftracer::utils::utilities::composites::dft::indexing::queries

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_QUERIES_H
