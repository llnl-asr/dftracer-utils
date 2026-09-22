#ifndef DFTRACER_UTILS_DATAFRAME_SKETCH_H
#define DFTRACER_UTILS_DATAFRAME_SKETCH_H

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
//
// This is the one mergeable quantile/histogram sketch for the whole codebase:
// it lives at the dataframe layer so the columnar aggregation engine can feed
// it a SIMD-computed batch of bucket keys, and the higher trace/View layer
// reaches it through the compatibility alias in
// utilities/common/statistics/ddsketch.h.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace dftracer::utils::dataframe {

/// One occupied histogram bucket: [lower, upper) DDSketch bounds and its count.
/// A {0, 0} bin carries the count of exact-zero values.
struct HistogramBin {
    double lower;
    double upper;
    std::uint64_t count;
};

class DDSketch {
   public:
    static constexpr int MAX_BINS = 128;

    explicit DDSketch(double relative_accuracy = 0.01);

    void add(double value, double weight = 1.0);
    /// Add a value whose DDSketch bucket key was precomputed by
    /// `sketch_bucket_keys` (the SIMD bulk path). `key == INT32_MIN` marks an
    /// exact zero, matching the kernel's sentinel; `value` keeps min / max.
    void add_key(std::int32_t key, double value, std::uint32_t weight = 1);
    void merge(const DDSketch& other);
    double quantile(double q) const;
    void reset();

    /// Occupied buckets in ascending value order (empty buckets omitted), for a
    /// raw histogram. Bounds are the sketch's relative-error bucket bounds.
    std::vector<HistogramBin> bins() const;

    double log_gamma() const { return log_gamma_; }
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

    /// Collapsing dense store: fixed-size array with offset.
    /// Logical bin index `k` maps to store_[k - offset_].
    /// When the key range exceeds MAX_BINS, lowest bins are
    /// collapsed into store_[0]. A bin saturates at UINT32_MAX.
    std::array<std::uint32_t, MAX_BINS> store_{};
    int offset_ = 0;
    int min_key_ = 0;
    int max_key_ = 0;
    bool initialized_ = false;
    bool collapsed_ = false;
    int num_bins_ = 0;

    void add_to_bin(int index, std::uint32_t count);
    double bin_lower_bound(int index) const;
    double bin_upper_bound(int index) const;
    void collapse_to_fit(int new_max_key);
};

/// Exact-zero sentinel produced by `sketch_bucket_keys` and consumed by
/// `DDSketch::add_key`; a zero value has no logarithmic bucket.
constexpr std::int32_t SKETCH_ZERO_KEY =
    std::numeric_limits<std::int32_t>::min();

/// Vectorized bulk mapping of `n` values to DDSketch bucket keys
/// (`ceil(log(|x|) / log_gamma)`), the sketch's one costly per-value step. The
/// scatter into bins stays scalar (a histogram increment does not vectorize),
/// but this hoists every logarithm out of it. `log_gamma` must match the target
/// sketch (`DDSketch::log_gamma()`); exact zeros map to `SKETCH_ZERO_KEY`.
void sketch_bucket_keys(const double* values, std::int64_t n, double log_gamma,
                        std::int32_t* out_keys);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_SKETCH_H
