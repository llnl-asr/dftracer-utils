#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_TYPES_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_TYPES_H

#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/index_file_entry_capability.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::indexer {

using ChunkBloomResult = composites::dft::indexing::queries::ChunkBloomResult;
using FileBloomResult = composites::dft::indexing::queries::FileBloomResult;
using ChunkStatisticsResult =
    composites::dft::indexing::queries::ChunkStatisticsResult;
using TimeBounds = composites::dft::indexing::queries::TimeBounds;
using EventRangeResult = composites::dft::indexing::queries::EventRangeResult;
using MetadataLinesResult =
    composites::dft::indexing::queries::MetadataLinesResult;
using ChunkStatistics = composites::dft::indexing::ChunkStatistics;
using ChunkDimensionStats = composites::dft::indexing::ChunkDimensionStats;
using ChunkDimensionStatsResult =
    composites::dft::indexing::ChunkDimensionStatsResult;
using IndexerCheckpoint = internal::IndexerCheckpoint;

struct MergedStatisticsResult {
    ChunkStatistics stats;
    std::uint64_t num_chunks = 0;
};

struct RootStatisticsResult {
    ChunkStatistics stats;
    std::uint64_t num_chunks = 0;
    std::uint64_t num_files = 0;
    std::uint64_t total_lines = 0;
    std::uint64_t total_uncompressed_bytes = 0;
};

struct NameSummaryResult {
    StringViewMap<std::uint64_t> counts;
    std::uint64_t other_count = 0;
    std::uint64_t unique_count = 0;
};

struct FileMetadataResult {
    std::uint64_t checkpoint_size = 0;
    std::uint64_t num_lines = 0;
    std::uint64_t max_bytes = 0;
};

struct FileRegistryEntry {
    int file_id = -1;
    IndexFileEntryCapability capabilities = IndexFileEntryCapability::NONE;
};

struct TarArchiveMetadata {
    std::string archive_name;
    std::uint64_t checkpoint_size = 0;
    std::uint64_t total_lines = 0;
    std::uint64_t total_uc_size = 0;
    std::uint64_t total_files = 0;
};

struct TarFileRecord {
    std::string file_name;
    std::uint64_t file_size = 0;
    std::uint64_t file_mtime = 0;
    char typeflag = '\0';
    std::uint64_t data_offset = 0;
    std::uint64_t uncompressed_offset = 0;
};

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_TYPES_H
