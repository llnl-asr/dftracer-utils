#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATOR_SUMMARY_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATOR_SUMMARY_UTILITY_H

#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_output.h>

namespace dftracer::utils::utilities::composites::dft::aggregators {

using AggregatorSummaryInput = EventAggregatorUtilityOutput;
using AggregatorSummaryOutput = void;

class AggregatorSummaryUtility
    : public utilities::Utility<AggregatorSummaryInput,
                                AggregatorSummaryOutput> {
   public:
    coro::CoroTask<void> process(const AggregatorSummaryInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATOR_SUMMARY_UTILITY_H
