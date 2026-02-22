#ifndef DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_DDSKETCH_H
#define DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_DDSKETCH_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::common::statistics {

/// Deterministic, merge-order-independent percentile estimation using
/// fixed logarithmic bins. Merges are commutative -- A+B+C always produces
/// the same result regardless of order.
///
/// Memory: O(log(max/min) / log(gamma)) bins -- bounded by the value range,
/// not the number of inserted elements.
/// Accuracy: bounded relative error (default 1%).
class DDSketch {
   public:
    explicit DDSketch(double relative_accuracy = 0.01);

    void add(double value, double weight = 1.0);
    void merge(const DDSketch& other);
    double quantile(double q) const;
    void reset();

    uint64_t count() const { return count_; }
    bool empty() const { return count_ == 0; }
    double min() const { return min_; }
    double max() const { return max_; }
    std::size_t memory_usage() const;

   private:
    double gamma_;
    double log_gamma_;
    double min_;
    double max_;
    uint64_t count_;
    uint64_t zero_count_;

    // Sparse bins: sorted by bin index for cache-friendly access and
    // binary-search insert/lookup.
    std::vector<std::pair<int, uint64_t>> bins_;

    int bin_index(double value) const;
    double bin_lower_bound(int index) const;
    double bin_upper_bound(int index) const;

    // Insert or increment a bin. Returns iterator to the affected bin.
    void add_to_bin(int index, uint64_t count);
};

}  // namespace dftracer::utils::utilities::common::statistics

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_DDSKETCH_H
