#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_OUTPUT_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_OUTPUT_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_map.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::index::schemas::dft::agg {

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

using BoundaryTimeRangeMap = StringViewMap<BoundaryTimeRange>;
using BoundaryTimeRangesMap = StringViewMap<BoundaryTimeRangeMap>;

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
    /// The table the keys' string ids belong to.
    AggInternPtr intern;
    AggregationMap aggregations;
    AggregationMap profile_aggregations;
    AggregationMap system_aggregations;
    std::size_t total_events_processed = 0;
    std::size_t total_files_processed = 0;
    std::size_t total_bytes_processed = 0;
    std::vector<std::shared_ptr<AssociationTracker>> trackers;
    bool success = true;

    const StringIntern& strings() const {
        if (!intern) {
            throw DFTUtilsException(ErrorCode::INTERNAL,
                                    "aggregation output has no intern table");
        }
        return intern->intern;
    }
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_OUTPUT_H
