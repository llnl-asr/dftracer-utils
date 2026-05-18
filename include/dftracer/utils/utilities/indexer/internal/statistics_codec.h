#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_STATISTICS_CODEC_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_STATISTICS_CODEC_H

#include <dftracer/utils/utilities/indexer/index_types.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::indexer::internal {

std::string encode_file_scalar_stats_value(const ChunkStatistics& stats,
                                           std::uint64_t num_chunks);

std::string encode_root_scalar_stats_value(
    const ChunkStatistics& stats, std::uint64_t num_chunks,
    std::uint64_t num_files, std::uint64_t total_lines = 0,
    std::uint64_t total_uncompressed_bytes = 0);

MergedStatisticsResult decode_file_scalar_stats_value(std::string_view value);

RootStatisticsResult decode_root_scalar_stats_value(std::string_view value);

}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_STATISTICS_CODEC_H
