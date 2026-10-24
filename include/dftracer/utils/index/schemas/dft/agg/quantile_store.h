#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_QUANTILE_STORE_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_QUANTILE_STORE_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::index::schemas::dft::agg {

/// Mergeable quantiles of one metric. Up to EXACT_CAPACITY values are kept
/// exactly (sorted); the next value turns the store into a MergeSketch of
/// the given relative accuracy, sized once from the key range of the values.
/// Stores that merge must share one accuracy.
///
/// Layout: a 24 byte union of the exact buffer and the sketch pointer, the
/// accuracy and a tag, 40 bytes in all. The exact buffer is a dstd::Vector
/// that grows from the first add, so a store of few values holds few bytes
/// and an empty store holds none; 64 inline doubles (512 B) per metric per
/// (chunk, category, name) would be too much.
class QuantileStore {
   public:
    static constexpr std::size_t EXACT_CAPACITY = 64;
    static constexpr double DEFAULT_ACCURACY = 0.01;

    explicit QuantileStore(double relative_accuracy = DEFAULT_ACCURACY);
    QuantileStore(QuantileStore&& other) noexcept;
    QuantileStore& operator=(QuantileStore&& other) noexcept;
    QuantileStore(const QuantileStore&) = delete;
    QuantileStore& operator=(const QuantileStore&) = delete;
    ~QuantileStore();

    /// A NaN is ignored.
    void add(double value);

    /// Adds every value of `values` (NaNs ignored). On a sketch the bucket
    /// keys come from the SIMD bulk kernel, in blocks with no allocation.
    void add_many(dstd::Span<const double> values);

    /// Merges `other` in place; `other` must not be `*this`. The result is
    /// exact only when both are exact and the total fits EXACT_CAPACITY.
    void merge(const QuantileStore& other);

    /// Exact store: linear interpolation between the two nearest ranks at
    /// position q * (n - 1), the rule of dataframe::quantile
    /// (dataframe/kernels/stats.cpp); q is clamped to [0, 1]. Sketch store:
    /// MergeSketch::quantile. NaN when empty.
    double quantile(double q) const;

    std::uint64_t count() const;

    /// Adds the values to `out`: each exact value, or for a sketch store a
    /// value inside each occupied bin with the bin's count, so `out` has the
    /// bins of the store (to within `out`'s own bin limit).
    void add_to(dataframe::DDSketch& out) const;
    bool is_exact() const noexcept { return !sketch_mode_; }
    double relative_accuracy() const noexcept { return accuracy_; }

    /// Appends to `buf`: tag byte, accuracy, then the values (exact) or the
    /// sketch bytes (sketch). Integers and the exact doubles are little
    /// endian; the sketch bytes keep the sketch's own byte order.
    void serialize_into(dstd::Vector<std::uint8_t>& buf) const;
    static expected<QuantileStore, ErrorCode> deserialize(
        const std::uint8_t* data, std::size_t len);

   private:
    using SketchPtr = dstd::UniquePtr<dataframe::MergeSketch>;
    using ExactVec = dstd::Vector<double>;

    /// Turns an exact store into a sketch holding its values and the NaN-free
    /// `extra`, in one sizing of the bins.
    void to_sketch(const double* extra, std::size_t n);
    void destroy() noexcept;

    union {
        ExactVec exact_;
        SketchPtr sketch_;
    };
    double accuracy_;
    bool sketch_mode_ = false;
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_QUANTILE_STORE_H
