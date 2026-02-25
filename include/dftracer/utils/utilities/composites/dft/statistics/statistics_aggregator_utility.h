#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_AGGREGATOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_AGGREGATOR_UTILITY_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/trace_statistics.h>

#include <string>

namespace dftracer::utils::utilities::composites::dft::statistics {

struct StatisticsAggregatorInput {
    std::string file_path;
    std::string bidx_path;
    std::string index_dir;
};

class StatisticsAggregatorUtility
    : public utilities::Utility<StatisticsAggregatorInput, TraceStatistics,
                                utilities::tags::Parallelizable> {
   public:
    StatisticsAggregatorUtility() = default;

    coro::CoroTask<TraceStatistics> process(
        const StatisticsAggregatorInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::statistics

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_AGGREGATOR_UTILITY_H
