#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_DRAIN_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_DRAIN_H

#include <dftracer/utils/utilities/composites/dft/dft_event_visitor.h>

#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

class EventAggregator;

// Drain the per-file extra visitors produced by the index batch builder into
// `merger`: for each AggregationVisitor, propagate its observed
// extra-key/custom-metric sets, take its chunk output, and merge it; then
// clear each file's visitor list. Returns the processed file paths (callers
// that don't need them can ignore the result). No-op returning an empty vector
// when `merger` is null. The drain is synchronous (no I/O), so it is safe to
// call from inside a coroutine without suspending.
std::vector<std::string> merge_aggregation_visitors(
    std::vector<std::vector<std::unique_ptr<DftEventVisitor>>>& extra_visitors,
    EventAggregator* merger);

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_DRAIN_H
