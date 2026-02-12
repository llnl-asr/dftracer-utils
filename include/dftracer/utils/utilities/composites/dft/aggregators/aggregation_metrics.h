#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_METRICS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_METRICS_H

#include <dftracer/utils/utilities/composites/dft/aggregators/ddsketch.h>

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>

namespace dftracer::utils::utilities::composites::dft::aggregators {

struct MetricStats {
    std::uint64_t total = 0;
    std::uint64_t min = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max = 0;
    double mean = 0.0;
    double m2 = 0.0;
    double m3 = 0.0;
    double m4 = 0.0;
    DDSketch sketch;

    explicit MetricStats(double relative_accuracy = 0.01)
        : sketch(relative_accuracy) {}

    void update(std::uint64_t value, std::uint64_t count,
                bool compute_percentiles = false);
    void merge_from(const MetricStats& other, std::uint64_t n1,
                    std::uint64_t n2, std::uint64_t n);
    double get_stddev(std::uint64_t count) const;
    double get_skewness(std::uint64_t count) const;
    double get_kurtosis(std::uint64_t count) const;
};

struct AggregationMetrics {
    std::uint64_t count = 0;

    MetricStats duration;
    MetricStats size;

    std::uint64_t ts = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t te = 0;

    std::unordered_map<std::string, std::string> boundary_associations;
    std::uint64_t parent_pid = 0;

    std::unordered_map<std::string, MetricStats> custom_metrics;

    double sketch_accuracy = 0.01;

    explicit AggregationMetrics(double relative_accuracy = 0.01)
        : duration(relative_accuracy),
          size(relative_accuracy),
          sketch_accuracy(relative_accuracy) {}

    void update_duration(std::uint64_t dur, bool compute_percentiles = false);
    void update_size(std::uint64_t sz, bool compute_percentiles = false);
    void update_timestamp(std::uint64_t event_ts, std::uint64_t dur);
    void update_timestamp_clamped(std::uint64_t event_ts, std::uint64_t dur,
                                  std::uint64_t bucket_start,
                                  std::uint64_t bucket_size);
    void update_custom_metric(const std::string& name, std::uint64_t value,
                              bool compute_percentiles = false);

    double get_stddev_duration() const;
    double get_stddev_size() const;
    double get_custom_stddev(const std::string& name) const;

    void merge_from(const AggregationMetrics& other);
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_METRICS_H
