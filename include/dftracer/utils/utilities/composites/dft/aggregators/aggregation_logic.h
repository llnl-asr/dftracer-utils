#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_LOGIC_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_LOGIC_H

#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_key.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_map.h>
#include <dftracer/utils/utilities/composites/dft/event.h>

#include <cstdint>

namespace dftracer::utils::utilities::composites::dft::aggregators {

std::uint64_t compute_time_bucket(std::uint64_t timestamp,
                                  std::uint64_t duration,
                                  const AggregationConfig& config);

AggregationKey build_aggregation_key(const DFTracerEvent& ev,
                                     const AggregationConfig& config);

void update_aggregation_entry(const DFTracerEvent& ev,
                              const AggregationConfig& config,
                              AggregationMap& aggregations,
                              const AggregationKey& key);

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_LOGIC_H
