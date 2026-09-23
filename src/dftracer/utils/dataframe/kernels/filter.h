#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_FILTER_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_FILTER_H

#include <dftracer/utils/dataframe/buffer.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/scalar.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace dftracer::utils::dataframe {

/// Rows of `v` whose value is greater than `threshold`, returned as a SELECTION
/// column over `v` (an index list; no value data is copied). `threshold` may be
/// any numeric type; it is converted to the column's element type, exactly for
/// 64-bit integers.
template <class T>
inline Series filter_gt(const Series& v, T threshold) {
    return Series{dftu_series_filter_gt(v.handle(), to_scalar(threshold))};
}

/// Rows of `v` where the bit-packed Bool `mask` (same length) is true, as a
/// SELECTION column over `v`. Composes with the comparison/string kernels that
/// produce Bool masks. Invalid (empty) if `mask` is not a matching Bool column.
Series filter(const Series& v, const Series& mask);

/// The row indices of the set bits of `bits` as an Int64 buffer, `count`
/// of them.
std::shared_ptr<Buffer> mask_to_indices(const std::uint8_t* bits,
                                        std::int64_t n, std::int64_t& count);
/// The same as an Int64 index column, for take().
Series mask_index_column(const std::uint8_t* bits, std::int64_t n);

/// Resolve any encoding to a new FLAT column (gather through the indirection).
Series materialize(const Series& v);

/// Gather the rows of `v` at `indices` into a new FLAT column. Handles every
/// column type (nested List/Struct included) and propagates validity.
Series take(const Series& v, const std::vector<std::int64_t>& indices);
/// The same over 32-bit indices, for a caller whose rows fit (a join whose
/// sides are under 2^31 rows): half the index traffic, the same gather.
Series take32(const Series& v, const std::vector<std::int32_t>& indices);
/// Every column of a frame gathered by the same `n` indices, the index list
/// scanned once for all of them (its bound and its null sentinels).
std::vector<Series> take_all(const std::vector<Series>& columns,
                             const std::int64_t* idx, std::int64_t n);
/// The rows of `v` at `indices` as a SELECTION view over `v` (its flat root:
/// a view over a view composes), no value data copied. What a frame filter
/// hands each column, so a column the caller never reads is never gathered.
Series select_rows(const Series& v, const std::vector<std::int64_t>& indices);
/// The same over every column of a frame, the index buffer shared.
std::vector<Series> select_rows(const std::vector<Series>& columns,
                                const std::vector<std::int64_t>& indices);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_FILTER_H
