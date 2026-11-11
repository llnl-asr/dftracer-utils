// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_AGG_QUANTILE_STORE_H
#define DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_AGG_QUANTILE_STORE_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::trace::index::schemas::dft::agg {

/// Mergeable quantiles of one metric. Up to EXACT_CAPACITY values are kept
/// exactly. The next value turns the store into a MergeSketch of the given
/// relative accuracy. Stores that merge must share one accuracy. The exact
/// buffer grows from the first add, as there is one store per metric per chunk.
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

    /// Adds every value of `values` (NaNs ignored).
    void add_many(dstd::Span<const double> values);

    /// Merges `other` in place; `other` must not be `*this`. The result is
    /// exact only when both are exact and the total fits EXACT_CAPACITY.
    void merge(const QuantileStore& other);

    /// Exact store: linear interpolation at position q * (n - 1), the rule of
    /// dataframe::quantile. Sketch store: MergeSketch::quantile. q is clamped
    /// to [0, 1]. NaN when empty.
    double quantile(double q) const;

    std::uint64_t count() const;

    /// Adds each exact value, or for a sketch store a value inside each
    /// occupied bin with the bin's count (to within `out`'s own bin limit).
    void add_to(dataframe::DDSketch& out) const;
    bool is_exact() const noexcept { return !sketch_mode_; }
    double relative_accuracy() const noexcept { return accuracy_; }

    /// Appends a tag byte, the accuracy, then the values (exact) or the sketch
    /// bytes. Integers and exact doubles are little endian. The sketch bytes
    /// keep the sketch's own byte order.
    void serialize_into(dstd::Vector<std::uint8_t>& buf) const;
    static dstd::Result<QuantileStore> deserialize(const std::uint8_t* data,
                                                   std::size_t len);

   private:
    using SketchPtr = dstd::UniquePtr<dataframe::MergeSketch>;
    using ExactVec = dstd::Vector<double>;

    void to_sketch(const double* extra, std::size_t n);
    void reserve_exact(std::size_t need);
    void destroy() noexcept;

    union {
        ExactVec exact_;
        SketchPtr sketch_;
    };
    double accuracy_;
    bool sketch_mode_ = false;
};

}  // namespace dftracer::utils::trace::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_AGG_QUANTILE_STORE_H
