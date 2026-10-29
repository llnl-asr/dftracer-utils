#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_drain.h>

#include <algorithm>

namespace dftracer::utils::index::schemas::dft::agg {

void merge_aggregation_folds(std::vector<AggFoldOutput>& outputs,
                             index::store::IndexDatabase& db) {
    std::uint64_t min_bucket = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_bucket = 0;
    for (auto& out : outputs) {
        min_bucket = std::min(min_bucket, out.min_time_bucket);
        max_bucket = std::max(max_bucket, out.max_time_bucket);
    }

    // min == UINT64_MAX is the only "no events seen" sentinel; a real bucket
    // range can legitimately be [0, 0] (relative time, first bucket).
    if (min_bucket != std::numeric_limits<std::uint64_t>::max() &&
        min_bucket <= max_bucket)
        tier::widen_time_bounds(db, {min_bucket, max_bucket});
}

}  // namespace dftracer::utils::index::schemas::dft::agg
