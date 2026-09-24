#ifndef DFTRACER_UTILS_INDEX_STORE_INTERNAL_STATISTICS_CODEC_H
#define DFTRACER_UTILS_INDEX_STORE_INTERNAL_STATISTICS_CODEC_H

#include <dftracer/utils/index/store/types.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace dftracer::utils::index::store::internal {

std::string encode_file_scalar_stats_value(
    const index::schemas::dft::ChunkStatistics& stats,
    std::uint64_t num_chunks);

index::schemas::dft::MergedStatisticsResult decode_file_scalar_stats_value(
    std::string_view value);

}  // namespace dftracer::utils::index::store::internal

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_STATISTICS_CODEC_H
