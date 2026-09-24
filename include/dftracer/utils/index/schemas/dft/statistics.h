#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_STATISTICS_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_STATISTICS_H

#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/store/queries.h>

#include <cstdint>

namespace dftracer::utils::index::schemas::dft {

using ChunkStatistics = index::schemas::dft::ChunkStatistics;
using ChunkDimensionStats = index::extensions::ChunkDimensionStats;
using ChunkStatisticsResult = index::store::queries::ChunkStatisticsResult;

struct MergedStatisticsResult {
    ChunkStatistics stats;
    std::uint64_t num_chunks = 0;
};

}  // namespace dftracer::utils::index::schemas::dft

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_STATISTICS_H
