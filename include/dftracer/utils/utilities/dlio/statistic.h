#ifndef DFTRACER_UTILS_UTILITIES_DLIO_STATISTIC_H
#define DFTRACER_UTILS_UTILITIES_DLIO_STATISTIC_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/index/schemas/dft/agg/quantile_store.h>

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::dlio {

/// Lightweight min/max/mean/count accumulator with an optional quantile store
/// for quantile queries. When no store is attached, quantile() falls back to a
/// uniform interpolation between observed min and max.
class Statistic {
   public:
    using Quantiles = index::schemas::dft::agg::QuantileStore;

    void attach_quantiles(dstd::SharedPtr<const Quantiles> quantiles) {
        quantiles_ = std::move(quantiles);
    }

    void update(double value) {
        if (value < min_val_) min_val_ = value;
        if (value > max_val_) max_val_ = value;
        sum_ += value;
        ++count_;
        mean_ = sum_ / static_cast<double>(count_);
    }

    double quantile(double q) const {
        if (quantiles_ && quantiles_->count() > 0)
            return quantiles_->quantile(q);
        if (count_ == 0 || min_val_ == std::numeric_limits<double>::infinity())
            return 0.0;
        return min_val_ + q * (max_val_ - min_val_);
    }

    double min() const { return count_ == 0 ? 0.0 : min_val_; }
    double max() const { return count_ == 0 ? 0.0 : max_val_; }
    double mean() const { return mean_; }
    std::uint64_t count() const { return count_; }

   private:
    double min_val_ = std::numeric_limits<double>::infinity();
    double max_val_ = -std::numeric_limits<double>::infinity();
    double sum_ = 0.0;
    double mean_ = 0.0;
    std::uint64_t count_ = 0;
    dstd::SharedPtr<const Quantiles> quantiles_;
};

struct ComponentTimeMetrics {
    double union_time = 0.0;
    double accumulated_time = 0.0;
    std::uint64_t num_samples = 0;
    Statistic stats;

    double concurrency() const {
        return union_time > 0.0 ? accumulated_time / union_time : 0.0;
    }
};

struct Boundary {
    std::int64_t time;
    int delta;  ///< +1 start, -1 end
};

/// Sweep-line union of [start, end] intervals encoded as boundaries.
/// Times are in microseconds; return value is seconds.
double sweep_union(std::vector<Boundary>& boundaries);

}  // namespace dftracer::utils::utilities::dlio

#endif
