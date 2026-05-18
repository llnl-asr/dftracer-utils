#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_OUTPUT_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_OUTPUT_H

#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_map.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

enum class AggMapType : std::uint8_t {
    EVENT = 0,
    PROFILE = 1,
    SYSTEM = 2,
};

class AssociationTracker;

struct BoundaryTimeRange {
    std::uint64_t ts = 0;
    std::uint64_t te = 0;
};

using BoundaryTimeRangeMap = std::unordered_map<std::string, BoundaryTimeRange>;
using BoundaryTimeRangesMap =
    std::unordered_map<std::string, BoundaryTimeRangeMap>;

struct ChunkAggregationOutput {
    int chunk_index = 0;
    AggregationMap aggregations;
    AggregationMap profile_aggregations;
    AggregationMap system_aggregations;
    std::size_t events_processed = 0;
    std::size_t bytes_processed = 0;
    std::string file_path;
    bool success = false;
    std::shared_ptr<AssociationTracker> local_tracker;
    std::uint64_t min_time_bucket = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_time_bucket = 0;
};

struct EventAggregatorOutput {
    AggregationMap aggregations;
    AggregationMap profile_aggregations;
    AggregationMap system_aggregations;
    std::size_t total_events_processed = 0;
    std::size_t total_files_processed = 0;
    std::size_t total_bytes_processed = 0;
    std::vector<std::shared_ptr<AssociationTracker>> trackers;
    bool success = true;
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_OUTPUT_H
