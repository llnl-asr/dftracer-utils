#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_DRAIN_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_DRAIN_H

#include <dftracer/utils/index/store/database.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::index::schemas::dft::agg {

class AssociationTracker;

/// The out-of-band outputs an AggregationFold produces per file (the partials
/// and system rows are written to the index write by AggregationFold::write).
struct AggFoldOutput {
    std::string file_path;
    std::shared_ptr<AssociationTracker> tracker;
    std::uint64_t min_time_bucket = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_time_bucket = 0;
    std::size_t events_processed = 0;
};

/// Merges the per-file AggregationFold out-of-band outputs into the index of
/// `db`: the trackers are finalized and merged with the stored one, and the
/// stored time bounds are widened by the range of the events. Synchronous, so
/// safe to call from a coroutine without suspending.
void merge_aggregation_folds(std::vector<AggFoldOutput>& outputs,
                             index::store::RocksDatabase& db);

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_DRAIN_H
