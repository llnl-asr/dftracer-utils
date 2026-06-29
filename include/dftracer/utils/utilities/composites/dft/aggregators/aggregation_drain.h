#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_DRAIN_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_DRAIN_H

#include <dftracer/utils/utilities/composites/dft/dft_event_visitor.h>

#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

class EventAggregator;

// Merge the per-file AggregationVisitors into `merger` and return the processed
// file paths. No-op when `merger` is null. Synchronous, so safe to call from a
// coroutine without suspending.
std::vector<std::string> merge_aggregation_visitors(
    std::vector<std::vector<std::unique_ptr<DftEventVisitor>>>& extra_visitors,
    EventAggregator* merger);

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_DRAIN_H
