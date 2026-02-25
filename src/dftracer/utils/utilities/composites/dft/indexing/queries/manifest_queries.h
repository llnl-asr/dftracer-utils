#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_QUERIES_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_QUERIES_H

#include <dftracer/utils/core/sqlite/database.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteDatabase;

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

// --- Provenance operations ---

void insert_provenance_info(const SqliteDatabase& db, const std::string& key,
                            const std::string& value);

void insert_provenance_source(const SqliteDatabase& db, int file_info_id,
                              int source_idx, const std::string& path,
                              int num_checkpoints,
                              const std::string& event_hash);

void insert_provenance_group(const SqliteDatabase& db, const std::string& name,
                             const std::string& predicate);

void insert_provenance_segment(const SqliteDatabase& db, int source_idx,
                               int source_checkpoint, int output_line_start,
                               int output_line_end, int event_count);

struct ProvenanceSource {
    int source_idx;
    std::string path;
    int num_checkpoints;
    std::string event_hash;
};

std::vector<ProvenanceSource> query_provenance_sources(const SqliteDatabase& db,
                                                       int file_info_id);

struct ProvenanceSegment {
    int source_idx;
    int source_checkpoint;
    int output_line_start;
    int output_line_end;
    int event_count;
};

std::vector<ProvenanceSegment> query_provenance_segments(
    const SqliteDatabase& db, int source_idx);

std::vector<ProvenanceSegment> query_all_provenance_segments(
    const SqliteDatabase& db);

std::string query_provenance_info(const SqliteDatabase& db,
                                  const std::string& key);

std::string query_provenance_group_name(const SqliteDatabase& db);

std::string query_provenance_group_predicate(const SqliteDatabase& db);

}  // namespace
   // dftracer::utils::utilities::composites::dft::indexing::queries

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_QUERIES_H
