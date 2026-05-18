#ifndef DFTRACER_UTILS_UTILITIES_DLIO_STATISTIC_H
#define DFTRACER_UTILS_UTILITIES_DLIO_STATISTIC_H

#include <dftracer/utils/utilities/common/statistics/statistic.h>

#include <cstdint>
#include <vector>

namespace dftracer::utils::utilities::dlio {

using Statistic = dftracer::utils::utilities::common::statistics::Statistic;

struct ComponentTimeMetrics {
    double union_time = 0.0;
    double accumulated_time = 0.0;
    std::uint64_t num_samples = 0;
    Statistic stats;

    double concurrency() const {
        return union_time > 0.0 ? accumulated_time / union_time : 0.0;
    }
};

struct Boundary {
    std::int64_t time;
    int delta;  // +1 start, -1 end
};

// Sweep-line union of [start, end] intervals encoded as boundaries.
// Times are in microseconds; return value is seconds.
double sweep_union(std::vector<Boundary>& boundaries);

}  // namespace dftracer::utils::utilities::dlio

#endif
