#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATOR_UTILITY_H

#include <dftracer/utils/core/utilities/streaming_utility.h>
#include <dftracer/utils/core/utilities/tags/needs_context.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregator_types.h>

#include <cstddef>
#include <optional>
#include <string>

namespace dftracer::utils::utilities::composites::dft::aggregators {

struct AggregatorInput {
    std::string directory;
    AggregationConfig config;
    std::optional<common::query::Query> query;
    std::size_t checkpoint_size = 32 * 1024 * 1024;
    std::string index_dir;
    bool force_rebuild = false;
    std::size_t parallelism = 0;  // 0 = use all available threads
    std::size_t event_batch_size = 10000;

    AggregatorInput& with_directory(const std::string& dir);
    AggregatorInput& with_config(const AggregationConfig& cfg);
    AggregatorInput& with_checkpoint_size(std::size_t sz);
    AggregatorInput& with_index_dir(const std::string& dir);
    AggregatorInput& with_force_rebuild(bool force);
    AggregatorInput& with_parallelism(std::size_t n);
    AggregatorInput& with_event_batch_size(std::size_t sz);
};

class AggregatorUtility
    : public StreamingUtility<AggregatorInput, AggregationBatch,
                              tags::NeedsContext> {
   public:
    coro::AsyncGenerator<AggregationBatch> process(
        const AggregatorInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATOR_UTILITY_H
