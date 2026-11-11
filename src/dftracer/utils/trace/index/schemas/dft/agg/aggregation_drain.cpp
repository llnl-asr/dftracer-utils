// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_drain.h>

#include <algorithm>

namespace dftracer::utils::trace::index::schemas::dft::agg {

dstd::Result<void> merge_aggregation_folds(dstd::Vector<AggFoldOutput>& outputs,
                                           const trace::index::Index& index) {
    std::uint64_t min_bucket = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_bucket = 0;
    for (auto& out : outputs) {
        min_bucket = std::min(min_bucket, out.min_time_bucket);
        max_bucket = std::max(max_bucket, out.max_time_bucket);
    }

    // min == UINT64_MAX is the only "no events seen" sentinel. A real bucket
    // range can be [0, 0] (relative time, first bucket).
    if (min_bucket == std::numeric_limits<std::uint64_t>::max() ||
        min_bucket > max_bucket)
        return {};
    auto snapshot = index.snapshot();
    if (!snapshot) return snapshot.error();
    auto stored = tier::read_time_bounds(*snapshot);
    if (!stored) return stored.error();
    if (*stored) {
        min_bucket = std::min(min_bucket, (*stored)->min_bucket);
        max_bucket = std::max(max_bucket, (*stored)->max_bucket);
    }
    return tier::write_time_bounds(index, {min_bucket, max_bucket});
}

}  // namespace dftracer::utils::trace::index::schemas::dft::agg
