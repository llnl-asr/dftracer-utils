#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_MAP_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_MAP_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_key.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_metrics.h>

namespace dftracer::utils::index::schemas::dft::agg {

/// Segmented (not flat): the aggregation visitor caches a pointer to the last
/// entry across events, so entries must not move on insert.
using AggregationMap =
    ankerl::unordered_dense::segmented_map<AggregationKey, AggregationMetrics,
                                           AggregationKeyHash,
                                           AggregationKeyEqual>;

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_MAP_H
