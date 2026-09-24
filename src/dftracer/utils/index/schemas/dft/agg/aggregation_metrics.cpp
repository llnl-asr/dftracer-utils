#include <dftracer/utils/index/schemas/dft/agg/aggregation_metrics.h>

#include <algorithm>
#include <cmath>

namespace dftracer::utils::index::schemas::dft::agg {

void MetricStats::update(double value, bool compute_percentiles) {
    stat.add(value);
    if (compute_percentiles) {
        if (!sketch) sketch = std::make_unique<DDSketch>(sketch_accuracy_);
        sketch->add(value);
    }
}

void MetricStats::update(std::uint64_t value, bool compute_percentiles) {
    stat.add(value);  // keeps the exact uint64 domain (durations/timestamps)
    if (compute_percentiles) {
        if (!sketch) sketch = std::make_unique<DDSketch>(sketch_accuracy_);
        sketch->add(static_cast<double>(value));
    }
}

void MetricStats::merge_from(const MetricStats& other) {
    stat.merge(other.stat);
    if (other.sketch) {
        if (!sketch) sketch = std::make_unique<DDSketch>(sketch_accuracy_);
        sketch->merge(*other.sketch);
    }
}

// Central moments from the raw power sums (sumsq = sum x^2, m3 = sum x^3,
// m4 = sum x^4): M2 = sumsq - n*mu^2, plus the analogous M3/M4 identities.
static void central_moments(const dftracer::utils::dataframe::FieldStat& s,
                            double& M2, double& M3, double& M4, double& n,
                            double& mu) {
    n = static_cast<double>(s.n);
    mu = s.sum / n;
    M2 = s.sumsq - n * mu * mu;
    M3 = s.m3 - 3.0 * mu * s.sumsq + 2.0 * n * mu * mu * mu;
    M4 = s.m4 - 4.0 * mu * s.m3 + 6.0 * mu * mu * s.sumsq -
         3.0 * n * mu * mu * mu * mu;
    if (M2 < 0.0) M2 = 0.0;  // rounding can push nonneg moments slightly < 0
    if (M4 < 0.0) M4 = 0.0;
}

double MetricStats::get_stddev() const {
    if (stat.n < 2) return 0.0;
    double M2, M3, M4, n, mu;
    central_moments(stat, M2, M3, M4, n, mu);
    const double var = M2 / (n - 1.0);
    return var > 0.0 ? std::sqrt(var) : 0.0;
}

double MetricStats::get_skewness() const {
    if (stat.n < 3) return 0.0;
    double M2, M3, M4, n, mu;
    central_moments(stat, M2, M3, M4, n, mu);
    if (M2 == 0.0) return 0.0;
    return std::sqrt(n) * M3 / std::pow(M2, 1.5);
}

double MetricStats::get_kurtosis() const {
    if (stat.n < 4) return 0.0;
    double M2, M3, M4, n, mu;
    central_moments(stat, M2, M3, M4, n, mu);
    if (M2 == 0.0) return 0.0;
    return n * M4 / (M2 * M2) - 3.0;
}

void AggregationMetrics::update_duration(std::uint64_t dur,
                                         bool compute_percentiles) {
    count++;
    duration.update(dur, compute_percentiles);
}

void AggregationMetrics::update_size(std::uint64_t sz,
                                     bool compute_percentiles) {
    size.update(sz, compute_percentiles);
}

void AggregationMetrics::update_offset(std::uint64_t off,
                                       bool compute_percentiles) {
    offset.update(off, compute_percentiles);
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

void AggregationMetrics::update_custom_metric(std::string_view name,
                                              std::uint64_t value,
                                              bool compute_percentiles) {
    if (!custom_metrics) {
        custom_metrics = std::make_unique<CustomMetricsMap>();
    }
    find_or_create(*custom_metrics, name, sketch_accuracy)
        .update(value, compute_percentiles);
}

void AggregationMetrics::merge_from(const AggregationMetrics& other) {
    count += other.count;

    duration.merge_from(other.duration);
    size.merge_from(other.size);
    offset.merge_from(other.offset);

    ts = std::min(ts, other.ts);
    te = std::max(te, other.te);

    distinct_files.merge_from(other.distinct_files);

    if (other.custom_metrics) {
        if (!custom_metrics) {
            custom_metrics = std::make_unique<CustomMetricsMap>();
        }
        for (const auto& [name, other_metric] : *other.custom_metrics) {
            find_or_create(*custom_metrics, name, sketch_accuracy)
                .merge_from(other_metric);
        }
    }
}

}  // namespace dftracer::utils::index::schemas::dft::agg
