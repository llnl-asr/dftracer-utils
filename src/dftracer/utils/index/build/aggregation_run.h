#ifndef DFTRACER_UTILS_INDEX_BUILD_AGGREGATION_RUN_H
#define DFTRACER_UTILS_INDEX_BUILD_AGGREGATION_RUN_H

#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/store/index_database.h>

#include <memory>
#include <string>
#include <utility>

namespace dftracer::utils::index::build {

/// Give `factory` to IndexBuildBatchConfig::agg_fold_factory, then call
/// finish_aggregation with the build's result.
struct AggRun {
    AggRun(std::string path,
           std::shared_ptr<index::store::IndexDatabase> database,
           IndexBuildBatchConfig::AggFoldFactory agg_factory);

    std::string index_path;
    std::shared_ptr<index::store::IndexDatabase> db;
    IndexBuildBatchConfig::AggFoldFactory factory;
};

AggRun start_aggregation(
    const std::string& index_path,
    const index::schemas::dft::agg::AggregationConfig& config);

void finish_aggregation(AggRun& run, IndexBuildBatchResult& batch);

/// Removes the tier of the index at @p index_path, so the next build does not
/// merge onto rows of other settings.
void clear_aggregation(const std::string& index_path);

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_AGGREGATION_RUN_H
