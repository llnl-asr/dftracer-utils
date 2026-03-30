#ifndef DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_DDSKETCH_H
#define DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_DDSKETCH_H

// DDSketch: A Fast and Fully-Mergeable Quantile Sketch with
// Relative-Error Guarantees.
//
// Based on the paper by Charles Masson, Jee E. Rim, Homin K. Lee
// (VLDB 2019): https://www.vldb.org/pvldb/vol12/p2195-masson.pdf
//
// The collapsing dense store implementation is adapted from the
// DataDog DDSketch reference implementations:
//   - Python: https://github.com/DataDog/sketches-py (Apache 2.0)
//   - Java:   https://github.com/DataDog/sketches-java (Apache 2.0)
//
// The store uses a fixed-size dense array with an offset. When the
// key range exceeds the bin limit, the lowest bins are collapsed
// (summed into the first bin) to bound memory usage.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace dftracer::utils::utilities::common::statistics {

class DDSketch {
   public:
    static constexpr int MAX_BINS = 128;

    explicit DDSketch(double relative_accuracy = 0.01);

    void add(double value, double weight = 1.0);
    void merge(const DDSketch& other);
    double quantile(double q) const;
    void reset();

    std::uint64_t count() const { return count_; }
    bool empty() const { return count_ == 0; }
    double min() const { return min_; }
    double max() const { return max_; }
    std::size_t memory_usage() const;

    std::vector<std::uint8_t> serialize() const;
    void serialize_into(std::vector<std::uint8_t>& buf) const;
    static DDSketch deserialize(const std::uint8_t* data, std::size_t len);

   private:
    double gamma_;
    double log_gamma_;
    double min_;
    double max_;
    std::uint64_t count_;
    std::uint64_t zero_count_;

    // Collapsing dense store: fixed-size array with offset.
    // Logical bin index `k` maps to store_[k - offset_].
    // When the key range exceeds MAX_BINS, lowest bins are
    // collapsed into store_[0].
    std::array<std::uint16_t, MAX_BINS> store_{};
    int offset_ = 0;
    int min_key_ = 0;
    int max_key_ = 0;
    bool initialized_ = false;
    bool collapsed_ = false;
    int num_bins_ = 0;

    int bin_index(double value) const;
    double bin_lower_bound(int index) const;
    double bin_upper_bound(int index) const;
    void add_to_bin(int index, std::uint16_t count);
    void collapse_to_fit(int new_max_key);
};

}  // namespace dftracer::utils::utilities::common::statistics

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_DDSKETCH_H
