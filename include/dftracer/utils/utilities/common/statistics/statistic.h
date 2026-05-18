#ifndef DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_STATISTIC_H
#define DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_STATISTIC_H

#include <dftracer/utils/utilities/common/statistics/ddsketch.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace dftracer::utils::utilities::common::statistics {

// Lightweight min/max/mean/count accumulator with an optional DDSketch backing
// for quantile queries. When no sketch is attached, quantile() falls back to a
// uniform interpolation between observed min and max.
class Statistic {
   public:
    Statistic() = default;

    void attach_sketch(std::shared_ptr<const DDSketch> sketch) {
        sketch_ = std::move(sketch);
    }

    void update(double value) {
        if (value < min_val_) min_val_ = value;
        if (value > max_val_) max_val_ = value;
        sum_ += value;
        ++count_;
        mean_ = sum_ / static_cast<double>(count_);
    }

    double quantile(double q) const {
        if (sketch_ && !sketch_->empty()) return sketch_->quantile(q);
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
    std::shared_ptr<const DDSketch> sketch_;
};

}  // namespace dftracer::utils::utilities::common::statistics

#endif
