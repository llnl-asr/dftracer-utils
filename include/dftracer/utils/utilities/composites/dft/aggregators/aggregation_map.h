#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_MAP_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_MAP_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_key.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_metrics.h>

namespace dftracer::utils::utilities::composites::dft::aggregators {

// Segmented (not flat): the aggregation visitor caches a pointer to the last
// entry across events, so entries must not move on insert.
using AggregationMap =
    ankerl::unordered_dense::segmented_map<AggregationKey, AggregationMetrics,
                                           AggregationKeyHash,
                                           AggregationKeyEqual>;

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_MAP_H
