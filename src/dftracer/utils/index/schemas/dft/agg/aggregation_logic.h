#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_LOGIC_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_LOGIC_H

#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_key.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_map.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/trace/event.h>

#include <cstdint>

namespace dftracer::utils::index::schemas::dft::agg {

/// The bucket an event starting at `timestamp` belongs to, as a View
/// time_bucket assigns it.
std::uint64_t compute_time_bucket(std::uint64_t timestamp,
                                  const AggregationConfig& config);

AggregationKey build_aggregation_key(const trace::DFTracerEvent& ev,
                                     const AggregationConfig& config,
                                     StringIntern& intern);

void update_aggregation_entry(const trace::DFTracerEvent& ev,
                              const AggregationConfig& config,
                              AggregationMap& aggregations,
                              const AggregationKey& key,
                              const StringIntern& intern);

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_LOGIC_H
