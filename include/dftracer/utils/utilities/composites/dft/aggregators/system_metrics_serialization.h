#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_SYSTEM_METRICS_SERIALIZATION_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_SYSTEM_METRICS_SERIALIZATION_H

#include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft::aggregators {

// Per-event-name keying so cpu/memory/etc keep separate buckets and the
// dfanalyzer side can pivot them into named columns (sys_cpu_idle_pct, ...).
struct SystemMetricKey {
    std::string hhash;
    std::string name;
    std::uint64_t time_bucket = 0;
};

void serialize_system_key_into(std::string& out, std::string_view hhash,
                               std::string_view name,
                               std::uint64_t time_bucket);
std::string serialize_system_key(std::string_view hhash, std::string_view name,
                                 std::uint64_t time_bucket);

struct DeserializedSystemKey {
    SystemMetricKey key;
};
DeserializedSystemKey deserialize_system_key(std::string_view data);

void serialize_system_value_into(std::string& out,
                                 const SystemAggregationMetrics& metrics);
std::string serialize_system_value(const SystemAggregationMetrics& metrics);
SystemAggregationMetrics deserialize_system_value(std::string_view data);

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_SYSTEM_METRICS_SERIALIZATION_H
