#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATOR_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATOR_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregator_types.h>
#include <dftracer/utils/query/query.h>

#include <cstddef>
#include <optional>
#include <string>

namespace dftracer::utils::index::schemas::dft::agg {

struct AggregatorInput {
    std::string directory;
    AggregationConfig config;
    std::optional<query::Query> query;
    std::size_t checkpoint_size = constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    std::string index_dir;
    bool force_rebuild = false;
    std::size_t parallelism = 0;  ///< 0 = use all available threads
    std::size_t event_batch_size = 10000;

    AggregatorInput& with_config(const AggregationConfig& cfg);
    AggregatorInput& with_checkpoint_size(std::size_t sz);
    AggregatorInput& with_index_dir(const std::string& dir);
    AggregatorInput& with_force_rebuild(bool force);
    AggregatorInput& with_event_batch_size(std::size_t sz);
};

class Aggregator {
   public:
    coro::AsyncGenerator<AggregationBatch> operator()(
        CoroScope& scope, const AggregatorInput& input) const;
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATOR_H
