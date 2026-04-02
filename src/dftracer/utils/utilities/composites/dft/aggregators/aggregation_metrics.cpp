#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_metrics.h>

#include <algorithm>
#include <cmath>

namespace dftracer::utils::utilities::composites::dft::aggregators {

void MetricStats::update(std::uint64_t value, std::uint64_t count,
                         bool compute_percentiles) {
    total += value;
    if (value < min) min = value;
    if (value > max) max = value;

    double n = static_cast<double>(count);
    double delta = static_cast<double>(value) - mean;
    double delta_n = delta / n;
    double delta_n2 = delta_n * delta_n;
    double term1 = delta * delta_n * (n - 1);

    m4 += term1 * delta_n2 * (n * n - 3 * n + 3) + 6 * delta_n2 * m2 -
          4 * delta_n * m3;
    m3 += term1 * delta_n * (n - 2) - 3 * delta_n * m2;
    m2 += term1;
    mean += delta_n;

    if (compute_percentiles) {
        if (!sketch) {
            sketch = std::make_unique<DDSketch>(sketch_accuracy_);
        }
        sketch->add(static_cast<double>(value));
    }
}

void MetricStats::merge_from(const MetricStats& other, std::uint64_t n1,
                             std::uint64_t n2, std::uint64_t n) {
    total += other.total;
    min = std::min(min, other.min);
    max = std::max(max, other.max);

    if (n > 0) {
        double delta = other.mean - mean;
        double delta2 = delta * delta;
        double delta3 = delta * delta2;
        double delta4 = delta2 * delta2;

        double n1_d = static_cast<double>(n1);
        double n2_d = static_cast<double>(n2);
        double n_d = static_cast<double>(n);

        double mean_new = (n1_d * mean + n2_d * other.mean) / n_d;

        m4 = m4 + other.m4 +
             delta4 * n1_d * n2_d * (n1_d * n1_d - n1_d * n2_d + n2_d * n2_d) /
                 (n_d * n_d * n_d) +
             6 * delta2 * (n1_d * n1_d * other.m2 + n2_d * n2_d * m2) /
                 (n_d * n_d) +
             4 * delta * (n1_d * other.m3 - n2_d * m3) / n_d;

        m3 = m3 + other.m3 +
             delta3 * n1_d * n2_d * (n1_d - n2_d) / (n_d * n_d) +
             3 * delta * (n1_d * other.m2 - n2_d * m2) / n_d;

        m2 = m2 + other.m2 + delta2 * n1_d * n2_d / n_d;

        mean = mean_new;
    }

    if (other.sketch) {
        if (!sketch) {
            sketch = std::make_unique<DDSketch>(sketch_accuracy_);
        }
        sketch->merge(*other.sketch);
    }
}

double MetricStats::get_stddev(std::uint64_t count) const {
    if (count < 2) return 0.0;
    return std::sqrt(m2 / static_cast<double>(count - 1));
}

double MetricStats::get_skewness(std::uint64_t count) const {
    if (count < 3 || m2 == 0.0) return 0.0;
    double n = static_cast<double>(count);
    return std::sqrt(n) * m3 / std::pow(m2, 1.5);
}

double MetricStats::get_kurtosis(std::uint64_t count) const {
    if (count < 4 || m2 == 0.0) return 0.0;
    double n = static_cast<double>(count);
    return n * m4 / (m2 * m2) - 3.0;
}

void AggregationMetrics::update_duration(std::uint64_t dur,
                                         bool compute_percentiles) {
    count++;
    duration.update(dur, count, compute_percentiles);
}

void AggregationMetrics::update_size(std::uint64_t sz,
                                     bool compute_percentiles) {
    size.update(sz, count, compute_percentiles);
}

void AggregationMetrics::update_timestamp(std::uint64_t event_ts,
                                          std::uint64_t dur) {
    if (event_ts < ts) ts = event_ts;
    std::uint64_t event_te = event_ts + dur;
    if (event_te > te) te = event_te;
}

void AggregationMetrics::update_timestamp_clamped(std::uint64_t event_ts,
                                                  std::uint64_t dur,
                                                  std::uint64_t bucket_start,
                                                  std::uint64_t bucket_size) {
    std::uint64_t bucket_end = bucket_start + bucket_size;
    std::uint64_t event_te = event_ts + dur;

    std::uint64_t clamped_ts = std::max(event_ts, bucket_start);
    if (clamped_ts < ts) ts = clamped_ts;

    std::uint64_t clamped_te = std::min(event_te, bucket_end);
    if (clamped_te > te) te = clamped_te;
}

void AggregationMetrics::update_custom_metric(const std::string& name,
                                              std::uint64_t value,
                                              bool compute_percentiles) {
    if (!custom_metrics) {
        custom_metrics = std::make_unique<CustomMetricsMap>();
    }
    if (custom_metrics->find(name) == custom_metrics->end()) {
        custom_metrics->emplace(name, MetricStats(sketch_accuracy));
    }
    (*custom_metrics)[name].update(value, count, compute_percentiles);
}

double AggregationMetrics::get_stddev_duration() const {
    return duration.get_stddev(count);
}

double AggregationMetrics::get_stddev_size() const {
    return size.get_stddev(count);
}

double AggregationMetrics::get_custom_stddev(const std::string& name) const {
    if (!custom_metrics) return 0.0;
    auto it = custom_metrics->find(name);
    if (it == custom_metrics->end()) return 0.0;
    return it->second.get_stddev(count);
}

void AggregationMetrics::merge_from(const AggregationMetrics& other) {
    std::uint64_t n1 = count;
    std::uint64_t n2 = other.count;
    std::uint64_t n = n1 + n2;

    count = n;

    duration.merge_from(other.duration, n1, n2, n);
    size.merge_from(other.size, n1, n2, n);

    ts = std::min(ts, other.ts);
    te = std::max(te, other.te);

    if (other.custom_metrics) {
        if (!custom_metrics) {
            custom_metrics = std::make_unique<CustomMetricsMap>();
        }
        for (const auto& [name, other_metric] : *other.custom_metrics) {
            (*custom_metrics)[name].merge_from(other_metric, n1, n2, n);
        }
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
