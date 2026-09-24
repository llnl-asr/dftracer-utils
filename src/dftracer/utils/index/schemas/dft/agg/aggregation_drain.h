#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_DRAIN_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_DRAIN_H

#include <dftracer/utils/core/common/transparent_string_hash.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::index::schemas::dft::agg {

class EventAggregator;
class AssociationTracker;

/// The out-of-band outputs an AggregationFold produces per file (the metrics
/// themselves are written to the index write by AggregationFold::write).
struct AggFoldOutput {
    std::string file_path;
    StringViewSet observed_extra_keys;
    StringViewSet observed_custom_metrics;
    std::shared_ptr<AssociationTracker> tracker;
    std::uint64_t min_time_bucket = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_time_bucket = 0;
    std::size_t events_processed = 0;
};

/// Merge the per-file AggregationFold out-of-band outputs into `merger`.
/// Finalizes each tracker before merging. No-op when `merger` is null.
/// Synchronous, so safe to call from a coroutine without suspending.
void merge_aggregation_folds(std::vector<AggFoldOutput>& outputs,
                             EventAggregator* merger);

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_DRAIN_H
