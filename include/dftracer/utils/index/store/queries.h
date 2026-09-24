#ifndef DFTRACER_UTILS_TRACE_INDEXING_QUERIES_H
#define DFTRACER_UTILS_TRACE_INDEXING_QUERIES_H

#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace dftracer::utils::index::store::queries {

struct ChunkStatisticsResult {
    std::uint64_t checkpoint_idx;
    index::schemas::dft::ChunkStatistics stats;
};

struct TimeBounds {
    std::uint64_t min_timestamp_us = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_timestamp_us = 0;
    bool valid = false;
};

}  // namespace dftracer::utils::index::store::queries

#endif  // DFTRACER_UTILS_TRACE_INDEXING_QUERIES_H
