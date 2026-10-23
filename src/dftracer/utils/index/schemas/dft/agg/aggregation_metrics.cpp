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

// The central moments the FieldStat already keeps (sums of (x - mean)^k).
static void central_moments(const dftracer::utils::dataframe::FieldStat& s,
                            double& M2, double& M3, double& M4, double& n,
                            double& mu) {
    n = static_cast<double>(s.n);
    mu = s.moment_mean();
    M2 = s.cm2;
    M3 = s.cm3;
    M4 = s.cm4;
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

}  // namespace dftracer::utils::index::schemas::dft::agg
