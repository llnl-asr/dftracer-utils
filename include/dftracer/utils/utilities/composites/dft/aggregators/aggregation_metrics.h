#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_METRICS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_METRICS_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/utilities/common/statistics/ddsketch.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>

namespace dftracer::utils::utilities::composites::dft::aggregators {

// Import DDSketch from common statistics
using common::statistics::DDSketch;

struct MetricStats {
    std::uint64_t count = 0;
    std::uint64_t total = 0;
    std::uint64_t min = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max = 0;
    double mean = 0.0;
    double m2 = 0.0;
    double m3 = 0.0;
    double m4 = 0.0;
    std::unique_ptr<DDSketch> sketch;
    double sketch_accuracy_ = 0.01;

    explicit MetricStats(double relative_accuracy = 0.01)
        : sketch_accuracy_(relative_accuracy) {}

    MetricStats(const MetricStats& other)
        : count(other.count),
          total(other.total),
          min(other.min),
          max(other.max),
          mean(other.mean),
          m2(other.m2),
          m3(other.m3),
          m4(other.m4),
          sketch(other.sketch ? std::make_unique<DDSketch>(*other.sketch)
                              : nullptr),
          sketch_accuracy_(other.sketch_accuracy_) {}

    MetricStats& operator=(const MetricStats& other) {
        if (this != &other) {
            count = other.count;
            total = other.total;
            min = other.min;
            max = other.max;
            mean = other.mean;
            m2 = other.m2;
            m3 = other.m3;
            m4 = other.m4;
            sketch = other.sketch ? std::make_unique<DDSketch>(*other.sketch)
                                  : nullptr;
            sketch_accuracy_ = other.sketch_accuracy_;
        }
        return *this;
    }

    MetricStats(MetricStats&&) = default;
    MetricStats& operator=(MetricStats&&) = default;

    void update(std::uint64_t value, bool compute_percentiles = false);
    void merge_from(const MetricStats& other);
    double get_stddev() const;
    double get_skewness() const;
    double get_kurtosis() const;
};

using CustomMetricsMap =
    std::unordered_map<std::string, MetricStats, TransparentStringHash,
                       TransparentStringEqual>;

struct AggregationMetrics {
    std::uint64_t count = 0;

    MetricStats duration;
    MetricStats size;

    std::uint64_t ts = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t te = 0;

    std::unique_ptr<std::unordered_map<std::string, std::string>>
        boundary_associations;
    std::uint64_t parent_pid = 0;

    std::unique_ptr<CustomMetricsMap> custom_metrics;

    double sketch_accuracy = 0.01;

    explicit AggregationMetrics(double relative_accuracy = 0.01)
        : duration(relative_accuracy),
          size(relative_accuracy),
          sketch_accuracy(relative_accuracy) {}

    AggregationMetrics(const AggregationMetrics& other)
        : count(other.count),
          duration(other.duration),
          size(other.size),
          ts(other.ts),
          te(other.te),
          boundary_associations(
              other.boundary_associations
                  ? std::make_unique<
                        std::unordered_map<std::string, std::string>>(
                        *other.boundary_associations)
                  : nullptr),
          parent_pid(other.parent_pid),
          custom_metrics(
              other.custom_metrics
                  ? std::make_unique<CustomMetricsMap>(*other.custom_metrics)
                  : nullptr),
          sketch_accuracy(other.sketch_accuracy) {}

    AggregationMetrics& operator=(const AggregationMetrics& other) {
        if (this != &other) {
            count = other.count;
            duration = other.duration;
            size = other.size;
            ts = other.ts;
            te = other.te;
            boundary_associations =
                other.boundary_associations
                    ? std::make_unique<
                          std::unordered_map<std::string, std::string>>(
                          *other.boundary_associations)
                    : nullptr;
            parent_pid = other.parent_pid;
            custom_metrics =
                other.custom_metrics
                    ? std::make_unique<CustomMetricsMap>(*other.custom_metrics)
                    : nullptr;
            sketch_accuracy = other.sketch_accuracy;
        }
        return *this;
    }

    AggregationMetrics(AggregationMetrics&&) = default;
    AggregationMetrics& operator=(AggregationMetrics&&) = default;

    void update_duration(std::uint64_t dur, bool compute_percentiles = false);
    void update_size(std::uint64_t sz, bool compute_percentiles = false);
    void update_timestamp(std::uint64_t event_ts, std::uint64_t dur);
    void update_timestamp_clamped(std::uint64_t event_ts, std::uint64_t dur,
                                  std::uint64_t bucket_start,
                                  std::uint64_t bucket_size);
    void update_custom_metric(std::string_view name, std::uint64_t value,
                              bool compute_percentiles = false);

    void merge_from(const AggregationMetrics& other);
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_METRICS_H
