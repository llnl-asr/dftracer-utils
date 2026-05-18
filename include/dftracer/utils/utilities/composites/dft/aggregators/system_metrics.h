#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_SYSTEM_METRICS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_SYSTEM_METRICS_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/utilities/common/statistics/ddsketch.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>

namespace dftracer::utils::utilities::composites::dft::aggregators {

using common::statistics::DDSketch;

struct FloatMetricStats {
    std::uint64_t count = 0;
    double total = 0.0;
    double min = std::numeric_limits<double>::max();
    double max = std::numeric_limits<double>::lowest();
    double mean = 0.0;
    double m2 = 0.0;
    std::unique_ptr<DDSketch> sketch;
    double sketch_accuracy_ = 0.01;

    explicit FloatMetricStats(double relative_accuracy = 0.01)
        : sketch_accuracy_(relative_accuracy) {}

    FloatMetricStats(const FloatMetricStats& other)
        : count(other.count),
          total(other.total),
          min(other.min),
          max(other.max),
          mean(other.mean),
          m2(other.m2),
          sketch(other.sketch ? std::make_unique<DDSketch>(*other.sketch)
                              : nullptr),
          sketch_accuracy_(other.sketch_accuracy_) {}

    FloatMetricStats& operator=(const FloatMetricStats& other) {
        if (this != &other) {
            count = other.count;
            total = other.total;
            min = other.min;
            max = other.max;
            mean = other.mean;
            m2 = other.m2;
            sketch = other.sketch ? std::make_unique<DDSketch>(*other.sketch)
                                  : nullptr;
            sketch_accuracy_ = other.sketch_accuracy_;
        }
        return *this;
    }

    FloatMetricStats(FloatMetricStats&&) = default;
    FloatMetricStats& operator=(FloatMetricStats&&) = default;

    void update(double value, bool compute_percentiles = false) {
        count++;
        total += value;
        if (value < min) min = value;
        if (value > max) max = value;

        // Welford's online mean/variance
        double delta = value - mean;
        mean += delta / static_cast<double>(count);
        double delta2 = value - mean;
        m2 += delta * delta2;

        if (compute_percentiles) {
            if (!sketch) {
                sketch = std::make_unique<DDSketch>(sketch_accuracy_);
            }
            sketch->add(value);
        }
    }

    void merge_from(const FloatMetricStats& other) {
        if (other.count == 0) return;
        if (count == 0) {
            *this = other;
            return;
        }

        std::uint64_t new_count = count + other.count;
        double delta = other.mean - mean;
        double new_mean = mean + delta * static_cast<double>(other.count) /
                                     static_cast<double>(new_count);
        double new_m2 = m2 + other.m2 +
                        delta * delta * static_cast<double>(count) *
                            static_cast<double>(other.count) /
                            static_cast<double>(new_count);

        count = new_count;
        total += other.total;
        if (other.min < min) min = other.min;
        if (other.max > max) max = other.max;
        mean = new_mean;
        m2 = new_m2;

        if (other.sketch) {
            if (!sketch) {
                sketch = std::make_unique<DDSketch>(*other.sketch);
            } else {
                sketch->merge(*other.sketch);
            }
        }
    }

    double get_stddev() const {
        if (count < 2) return 0.0;
        return std::sqrt(m2 / static_cast<double>(count - 1));
    }
};

using FloatMetricsMap =
    std::unordered_map<std::string, FloatMetricStats, TransparentStringHash,
                       TransparentStringEqual>;

struct SystemAggregationMetrics {
    std::uint64_t count = 0;

    // Timestamp bounds for this bucket
    std::uint64_t ts = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t te = 0;

    // Named system metrics (aggregated as mean per bucket)
    std::unique_ptr<FloatMetricsMap> metrics;

    double sketch_accuracy = 0.01;

    explicit SystemAggregationMetrics(double relative_accuracy = 0.01)
        : sketch_accuracy(relative_accuracy) {}

    SystemAggregationMetrics(const SystemAggregationMetrics& other)
        : count(other.count),
          ts(other.ts),
          te(other.te),
          metrics(other.metrics
                      ? std::make_unique<FloatMetricsMap>(*other.metrics)
                      : nullptr),
          sketch_accuracy(other.sketch_accuracy) {}

    SystemAggregationMetrics& operator=(const SystemAggregationMetrics& other) {
        if (this != &other) {
            count = other.count;
            ts = other.ts;
            te = other.te;
            metrics = other.metrics
                          ? std::make_unique<FloatMetricsMap>(*other.metrics)
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
            metrics = std::make_unique<FloatMetricsMap>();
        }
        auto it = metrics->find(name);
        if (it == metrics->end()) {
            it = metrics
                     ->emplace(std::string(name),
                               FloatMetricStats(sketch_accuracy))
                     .first;
        }
        it->second.update(value, compute_percentiles);
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
                metrics = std::make_unique<FloatMetricsMap>();
            }
            for (const auto& [name, stats] : *other.metrics) {
                auto it = metrics->find(name);
                if (it == metrics->end()) {
                    it = metrics
                             ->emplace(name, FloatMetricStats(sketch_accuracy))
                             .first;
                }
                it->second.merge_from(stats);
            }
        }
    }
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_SYSTEM_METRICS_H
