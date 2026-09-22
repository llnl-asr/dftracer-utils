#ifndef DFTRACER_UTILS_TRACE_INDEXING_QUERIES_H
#define DFTRACER_UTILS_TRACE_INDEXING_QUERIES_H

#include <dftracer/utils/trace/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/trace/indexing/chunk_statistics.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace dftracer::utils::trace::indexing::queries {

struct ChunkBloomResult {
    std::uint64_t checkpoint_idx;
    std::vector<unsigned char> bloom_data;
    std::uint64_t num_entries;
};

struct FileBloomResult {
    std::vector<unsigned char> bloom_data;
    std::uint64_t num_entries;
};

struct ChunkStatisticsResult {
    std::uint64_t checkpoint_idx;
    ChunkStatistics stats;
};

struct TimeBounds {
    std::uint64_t min_timestamp_us = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_timestamp_us = 0;
    bool valid = false;
};

}  // namespace dftracer::utils::trace::indexing::queries

#endif  // DFTRACER_UTILS_TRACE_INDEXING_QUERIES_H
