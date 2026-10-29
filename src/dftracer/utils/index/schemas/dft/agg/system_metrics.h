#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_SYSTEM_METRICS_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_SYSTEM_METRICS_H

#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_metrics.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

#include <cstdint>
#include <limits>
#include <string_view>

namespace dftracer::utils::index::schemas::dft::agg {

/// The number of shards the counters of a typed collect are split into.
static constexpr std::uint16_t AGG_KEY_NUM_SHARDS = 4096;

/// The shard of the counter group of host `hhash` and metric `name`: a
/// function of their texts alone, so the buckets of one group share a shard.
inline std::uint16_t system_shard(std::string_view hhash,
                                  std::string_view name) {
    utilities::hash::Fnv1aHashBuilder h;
    h.update(hhash);
    h.update(name);
    return static_cast<std::uint16_t>(h.finish() % AGG_KEY_NUM_SHARDS);
}

/// System metrics (CPU/GPU %, gauges) share the same MetricStats atom as event
/// metrics; they differ only in carrying fractional values.
using SystemMetricsMap = dstd::HashMap<dstd::String, MetricStats>;

/// The counters of one (host, name, time bucket) of a typed collect: the
/// count, the timestamp range and one MetricStats per numeric arg.
struct SystemAggregationMetrics {
    std::uint64_t count = 0;

    /// The timestamp range of the events of the group.
    std::uint64_t ts = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t te = 0;

    /// Named metrics of the group (aggregated as a mean per group)
    dstd::UniquePtr<SystemMetricsMap> metrics;

    double sketch_accuracy = 0.01;

    explicit SystemAggregationMetrics(double relative_accuracy = 0.01)
        : sketch_accuracy(relative_accuracy) {}

    SystemAggregationMetrics(const SystemAggregationMetrics&) = delete;
    SystemAggregationMetrics& operator=(const SystemAggregationMetrics&) =
        delete;
    SystemAggregationMetrics(SystemAggregationMetrics&&) = default;
    SystemAggregationMetrics& operator=(SystemAggregationMetrics&&) = default;

    void update_metric(std::string_view name, double value,
                       bool compute_percentiles = false) {
        if (!metrics) {
            metrics = dstd::make_unique<SystemMetricsMap>();
        }
        find_or_create(*metrics, name, sketch_accuracy)
            .update(value, compute_percentiles);
    }

    void update_timestamp(std::uint64_t event_ts) {
        if (event_ts < ts) ts = event_ts;
        if (event_ts > te) te = event_ts;
    }

    void merge_from(const SystemAggregationMetrics& other) {
        count += other.count;
        if (other.ts < ts) ts = other.ts;
        if (other.te > te) te = other.te;

        if (other.metrics) {
            if (!metrics) {
                metrics = dstd::make_unique<SystemMetricsMap>();
            }
            for (const auto& [name, stats] : *other.metrics) {
                find_or_create(*metrics, name, sketch_accuracy)
                    .merge_from(stats);
            }
        }
    }
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_SYSTEM_METRICS_H
