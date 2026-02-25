#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_EVENT_AGGREGATOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_EVENT_AGGREGATOR_UTILITY_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_output.h>

#include <string>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

struct EventAggregatorUtilityInput {
    std::vector<ChunkAggregationOutput> chunk_outputs;
};

class EventAggregatorUtility
    : public utilities::Utility<EventAggregatorUtilityInput,
                                EventAggregatorUtilityOutput> {
   public:
    coro::CoroTask<EventAggregatorUtilityOutput> process(
        const EventAggregatorUtilityInput& input) override;

    void merge_chunk(ChunkAggregationOutput&& chunk_output);
    EventAggregatorUtilityOutput finalize();

   private:
    EventAggregatorUtilityOutput state_;
    std::unordered_set<std::string> unique_files_;
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_EVENT_AGGREGATOR_UTILITY_H
