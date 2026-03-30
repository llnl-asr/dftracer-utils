#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_OUTPUT_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_OUTPUT_H

#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_map.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

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
    std::size_t events_processed = 0;
    std::size_t bytes_processed = 0;
    std::string file_path;
    bool success = false;
    std::shared_ptr<AssociationTracker> local_tracker;
};

struct EventAggregatorUtilityOutput {
    AggregationMap aggregations;
    std::size_t total_events_processed = 0;
    std::size_t total_files_processed = 0;
    std::size_t total_bytes_processed = 0;
    std::vector<std::shared_ptr<AssociationTracker>> trackers;
    bool success = true;
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_OUTPUT_H
