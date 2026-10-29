#ifndef DFTRACER_UTILS_INDEX_BUILD_AGGREGATION_RUN_H
#define DFTRACER_UTILS_INDEX_BUILD_AGGREGATION_RUN_H

#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/store/index_database.h>

#include <memory>
#include <string>

namespace dftracer::utils::index::build {

/// The aggregation of one build into one index's tier: give `factory` to the
/// build's IndexBuildBatchConfig::agg_fold_factory, then call
/// finish_aggregation with the build's result.
struct AggRun {
    std::shared_ptr<index::store::IndexDatabase> db;
    IndexBuildBatchConfig::AggFoldFactory factory;
};

/// Opens the tier of the index at @p index_path for a build under @p config:
/// loads its dictionary and records the config the files are folded with.
AggRun start_aggregation(
    const std::string& index_path,
    const index::schemas::dft::agg::AggregationConfig& config);

/// Persists what the folds of @p batch left out of band.
void finish_aggregation(AggRun& run, IndexBuildBatchResult& batch);

/// Removes the tier of the index at @p index_path, so the next build makes it
/// whole instead of merging onto rows of other settings.
void clear_aggregation(const std::string& index_path);

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_AGGREGATION_RUN_H
