#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_DRAIN_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_DRAIN_H

#include <dftracer/utils/index/store/index_database.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace dftracer::utils::index {
class Index;
}

namespace dftracer::utils::index::schemas::dft::agg {

/// The out-of-band outputs of an AggregationFold per file. The partials and
/// column blocks go through AggregationFold::write.
struct AggFoldOutput {
    std::string file_path;
    std::uint64_t min_time_bucket = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_time_bucket = 0;
    std::size_t events_processed = 0;
};

/// Widens the stored time bounds of `index` by the range of the events of
/// `outputs`. Synchronous, so safe to call from a coroutine without suspending.
/// `db` reads the bounds already stored.
void merge_aggregation_folds(std::vector<AggFoldOutput>& outputs,
                             index::store::IndexDatabase& db,
                             const index::Index& index);

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_DRAIN_H
