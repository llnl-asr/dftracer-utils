// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_DRAIN_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_DRAIN_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <limits>

namespace dftracer::utils::index {
class Index;
}

namespace dftracer::utils::index::schemas::dft::agg {

/// The out-of-band outputs of an AggregationFold per file. The partials and
/// column blocks go through AggregationFold::write.
struct AggFoldOutput {
    dstd::String file_path;
    std::uint64_t min_time_bucket = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_time_bucket = 0;
    std::size_t events_processed = 0;
};

/// Widens the stored time bounds of `index` by the range of the events of
/// `outputs`. Synchronous, so safe to call from a coroutine without suspending.
/// The bounds already stored are read from a snapshot of `index`. An error when
/// the index cannot be read or written.
dstd::Result<void> merge_aggregation_folds(dstd::Vector<AggFoldOutput>& outputs,
                                           const index::Index& index);

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_DRAIN_H
