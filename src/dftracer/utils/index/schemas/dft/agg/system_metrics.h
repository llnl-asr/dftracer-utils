// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_SYSTEM_METRICS_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_SYSTEM_METRICS_H

#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/hash/xxh3.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_metrics.h>

#include <cstdint>
#include <limits>
#include <string_view>

namespace dftracer::utils::index::schemas::dft::agg {

static constexpr std::uint16_t AGG_KEY_NUM_SHARDS = 4096;

/// Depends on the texts alone, so the buckets of one group share a shard.
inline std::uint16_t system_shard(dstd::StringView hhash,
                                  dstd::StringView name) {
    const std::uint64_t h = dstd::hash::splitmix64(
        dstd::hash::xxh3_64(hhash) ^
        dstd::hash::splitmix64(dstd::hash::xxh3_64(name)));
    return static_cast<std::uint16_t>(h % AGG_KEY_NUM_SHARDS);
}

/// System metrics (CPU/GPU %, gauges) share the same MetricStats atom as event
/// metrics; they differ only in carrying fractional values.
using SystemMetricsMap = dstd::HashMap<dstd::String, MetricStats>;

struct SystemAggregationMetrics {
    std::uint64_t count = 0;

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

    void update_metric(dstd::StringView name, double value,
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
