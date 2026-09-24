#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_SYSTEM_METRICS_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_SYSTEM_METRICS_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_metrics.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace dftracer::utils::index::schemas::dft::agg {

/// System metrics (CPU/GPU %, gauges) share the same MetricStats atom as event
/// metrics; they differ only in carrying fractional values.
using SystemMetricsMap =
    ankerl::unordered_dense::map<std::string, MetricStats,
                                 TransparentStringHash, TransparentStringEqual>;

struct SystemAggregationMetrics {
    std::uint64_t count = 0;

    /// Timestamp bounds for this bucket
    std::uint64_t ts = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t te = 0;

    /// Named system metrics (aggregated as mean per bucket)
    std::unique_ptr<SystemMetricsMap> metrics;

    double sketch_accuracy = 0.01;

    explicit SystemAggregationMetrics(double relative_accuracy = 0.01)
        : sketch_accuracy(relative_accuracy) {}

    SystemAggregationMetrics(const SystemAggregationMetrics& other)
        : count(other.count),
          ts(other.ts),
          te(other.te),
          metrics(other.metrics
                      ? std::make_unique<SystemMetricsMap>(*other.metrics)
                      : nullptr),
          sketch_accuracy(other.sketch_accuracy) {}

    SystemAggregationMetrics& operator=(const SystemAggregationMetrics& other) {
        if (this != &other) {
            count = other.count;
            ts = other.ts;
            te = other.te;
            metrics = other.metrics
                          ? std::make_unique<SystemMetricsMap>(*other.metrics)
                          : nullptr;
            sketch_accuracy = other.sketch_accuracy;
        }
        return *this;
    }

    SystemAggregationMetrics(SystemAggregationMetrics&&) = default;
    SystemAggregationMetrics& operator=(SystemAggregationMetrics&&) = default;

    void update_metric(std::string_view name, double value,
                       bool compute_percentiles = false) {
        if (!metrics) {
            metrics = std::make_unique<SystemMetricsMap>();
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
                metrics = std::make_unique<SystemMetricsMap>();
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
