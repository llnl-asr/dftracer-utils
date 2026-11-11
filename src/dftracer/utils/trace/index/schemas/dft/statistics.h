// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_STATISTICS_H
#define DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_STATISTICS_H

#include <dftracer/utils/trace/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/trace/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/trace/index/store/queries.h>

#include <cstdint>

namespace dftracer::utils::trace::index::schemas::dft {

using ChunkStatistics = trace::index::schemas::dft::ChunkStatistics;
using ChunkDimensionStats = trace::index::extensions::ChunkDimensionStats;
using ChunkStatisticsResult =
    trace::index::store::queries::ChunkStatisticsResult;

struct MergedStatisticsResult {
    ChunkStatistics stats;
    std::uint64_t chunk_count = 0;
};

}  // namespace dftracer::utils::trace::index::schemas::dft

#endif  // DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_STATISTICS_H
