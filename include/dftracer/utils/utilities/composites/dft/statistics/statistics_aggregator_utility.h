#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_AGGREGATOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_AGGREGATOR_UTILITY_H

#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/trace_statistics.h>

#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::statistics {

struct StatisticsAggregatorInput {
    std::string file_path;
    std::string index_path;
    std::string index_dir;
};

struct StatisticsAggregatorBatchInput {
    std::vector<std::string> file_paths;
    std::string index_path;
};

class StatisticsAggregatorUtility
    : public utilities::Utility<StatisticsAggregatorInput, TraceStatistics> {
   public:
    StatisticsAggregatorUtility() = default;

    coro::CoroTask<TraceStatistics> process(
        const StatisticsAggregatorInput& input) override;

    coro::CoroTask<std::vector<TraceStatistics>> process_batch(
        const StatisticsAggregatorBatchInput& input);
};

}  // namespace dftracer::utils::utilities::composites::dft::statistics

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_AGGREGATOR_UTILITY_H
