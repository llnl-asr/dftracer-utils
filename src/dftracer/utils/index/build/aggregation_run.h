// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_BUILD_AGGREGATION_RUN_H
#define DFTRACER_UTILS_INDEX_BUILD_AGGREGATION_RUN_H

#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>

#include <utility>

namespace dftracer::utils::index::build {

/// Give `factory` to IndexBuildBatchConfig::agg_fold_factory, then call
/// finish_aggregation with the build's result.
struct AggRun {
    AggRun(dstd::String path,
           IndexBuildBatchConfig::AggFoldFactory agg_factory);

    dstd::String index_path;
    IndexBuildBatchConfig::AggFoldFactory factory;
};

/// An error when the index cannot be opened or its aggregation config cannot
/// be written.
dstd::Result<AggRun> start_aggregation(
    dstd::StringView index_path,
    const index::schemas::dft::agg::AggregationConfig& config);

dstd::Result<void> finish_aggregation(AggRun& run,
                                      IndexBuildBatchResult& batch);

/// Removes the tier of the index at @p index_path, so the next build does not
/// merge onto rows of other settings.
dstd::Result<void> clear_aggregation(const dstd::String& index_path);

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_AGGREGATION_RUN_H
