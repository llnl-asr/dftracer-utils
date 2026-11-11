// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_STORE_QUERIES_H
#define DFTRACER_UTILS_TRACE_INDEX_STORE_QUERIES_H

#include <dftracer/utils/trace/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/trace/index/schemas/dft/chunk_statistics.h>

#include <cstdint>
#include <limits>

namespace dftracer::utils::trace::index::store::queries {

struct ChunkStatisticsResult {
    std::uint64_t checkpoint_idx;
    trace::index::schemas::dft::ChunkStatistics stats;
};

struct TimeBounds {
    std::uint64_t min_timestamp_us = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_timestamp_us = 0;
    bool valid = false;
};

}  // namespace dftracer::utils::trace::index::store::queries

#endif  // DFTRACER_UTILS_TRACE_INDEX_STORE_QUERIES_H
