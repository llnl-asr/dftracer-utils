#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_metrics.h>

#include <algorithm>
#include <cmath>

namespace dftracer::utils::utilities::composites::dft::aggregators {

// Representation note:
//   count, total -> plain integer running sums (bit-exact regardless of
//                   merge order; overflow guarded by u64 range for typical
//                   trace magnitudes).
//   m2, m3, m4   -> REPURPOSED. Now hold raw power sums:
//                     m2 = sum_x^2
//                     m3 = sum_x^3
//                     m4 = sum_x^4
//                   Instead of Welford central moments. Merge becomes
//                   plain addition, making it commutative + associative.
//                   Integer-valued inputs with v^k representable in
//                   double mantissa (<= 2^52) keep additions exact, so
//                   serial and MPI outputs match bit-for-bit. Stddev /
//                   skewness / kurtosis are computed at read time by
//                   converting power sums to central moments.
//   mean         -> Not maintained incrementally; filled in at emit time
//                   by the aggregator from (total / count).
void MetricStats::update(std::uint64_t value, bool compute_percentiles) {
    count++;
    total += value;
    if (value < min) min = value;
    if (value > max) max = value;

    const double v = static_cast<double>(value);
    const double v2 = v * v;
    m2 += v2;
    m3 += v2 * v;
    m4 += v2 * v2;
    mean = static_cast<double>(total) / static_cast<double>(count);

    if (compute_percentiles) {
        if (!sketch) {
            sketch = std::make_unique<DDSketch>(sketch_accuracy_);
        }
        sketch->add(v);
    }
}

void MetricStats::merge_from(const MetricStats& other) {
    count += other.count;
    total += other.total;
    min = std::min(min, other.min);
    max = std::max(max, other.max);
    m2 += other.m2;
    m3 += other.m3;
    m4 += other.m4;
    mean = count > 0 ? static_cast<double>(total) / static_cast<double>(count)
                     : 0.0;

    if (other.sketch) {
        if (!sketch) {
            sketch = std::make_unique<DDSketch>(sketch_accuracy_);
        }
        sketch->merge(*other.sketch);
    }
}

// Convert power sums (m2=sum_x^2, m3=sum_x^3, m4=sum_x^4) and
// (count, total) into the central moments needed for stddev / skewness
// / kurtosis. Well-known identities:
//   mu = total / n
//   M2 = sum_x^2 - n * mu^2
//   M3 = sum_x^3 - 3 * mu * sum_x^2 + 2 * n * mu^3
//   M4 = sum_x^4 - 4 * mu * sum_x^3 + 6 * mu^2 * sum_x^2 - 3 * n * mu^4
static void central_moments(std::uint64_t count, std::uint64_t total, double m2,
                            double m3, double m4, double& M2, double& M3,
                            double& M4, double& n, double& mu) {
    n = static_cast<double>(count);
    mu = static_cast<double>(total) / n;
    M2 = m2 - n * mu * mu;
    M3 = m3 - 3.0 * mu * m2 + 2.0 * n * mu * mu * mu;
    M4 = m4 - 4.0 * mu * m3 + 6.0 * mu * mu * m2 - 3.0 * n * mu * mu * mu * mu;
    // Rounding can push nonneg moments slightly negative.
    if (M2 < 0.0) M2 = 0.0;
    if (M4 < 0.0) M4 = 0.0;
}

double MetricStats::get_stddev() const {
    if (count < 2) return 0.0;
    double M2, M3, M4, n, mu;
    central_moments(count, total, m2, m3, m4, M2, M3, M4, n, mu);
    const double var = M2 / (n - 1.0);
    return var > 0.0 ? std::sqrt(var) : 0.0;
}

double MetricStats::get_skewness() const {
    if (count < 3) return 0.0;
    double M2, M3, M4, n, mu;
    central_moments(count, total, m2, m3, m4, M2, M3, M4, n, mu);
    if (M2 == 0.0) return 0.0;
    return std::sqrt(n) * M3 / std::pow(M2, 1.5);
}

double MetricStats::get_kurtosis() const {
    if (count < 4) return 0.0;
    double M2, M3, M4, n, mu;
    central_moments(count, total, m2, m3, m4, M2, M3, M4, n, mu);
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
    auto it = custom_metrics->find(name);
    if (it == custom_metrics->end()) {
        auto [new_it, _] = custom_metrics->emplace(
            std::string(name), MetricStats(sketch_accuracy));
        it = new_it;
    }
    it->second.update(value, compute_percentiles);
}

void AggregationMetrics::merge_from(const AggregationMetrics& other) {
    count += other.count;

    duration.merge_from(other.duration);
    size.merge_from(other.size);

    ts = std::min(ts, other.ts);
    te = std::max(te, other.te);

    if (other.custom_metrics) {
        if (!custom_metrics) {
            custom_metrics = std::make_unique<CustomMetricsMap>();
        }
        for (const auto& [name, other_metric] : *other.custom_metrics) {
            auto it = custom_metrics->find(name);
            if (it == custom_metrics->end()) {
                auto [new_it, _] =
                    custom_metrics->emplace(name, MetricStats(sketch_accuracy));
                it = new_it;
            }
            it->second.merge_from(other_metric);
        }
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
