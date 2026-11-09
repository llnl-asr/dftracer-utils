// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_REDUCE_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_REDUCE_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dstd/result.h>

#include <cstdint>

namespace dftracer::utils::dataframe {

/// Reduce a FLAT numeric column to a scalar, skipping nulls. `sum` accumulates
/// in the widest type of the column's domain (so it does not overflow narrow
/// integers or lose 64-bit precision); `min`/`max` return a value in the
/// column's domain. Read the result with scalar_value<T>(). A column type the
/// reduction is not defined for (a string sum) gives an InvalidArgument error.
dstd::Result<dftu_scalar> sum(const Series& v);
dstd::Result<dftu_scalar> min(const Series& v);
dstd::Result<dftu_scalar> max(const Series& v);

/// Number of non-null rows.
std::int64_t count(const Series& v);

/// Arithmetic mean of the non-null values, as a double (0 when empty); the
/// error of sum() for a type with no sum.
dstd::Result<double> mean(const Series& v);

/// Product of the non-null values, in the column's wide domain (like sum).
dftu_scalar product(const Series& v);

/// Boolean reductions over a Bool column, skipping nulls: `all` is true when
/// every non-null bit is set, `any` when at least one is.
bool all(const Series& v);
bool any(const Series& v);

/// Row index of the min / max non-null value (first on ties), or -1 if none.
std::int64_t arg_min(const Series& v);
std::int64_t arg_max(const Series& v);

/// Most frequent non-null value (hash-based; first-seen wins on ties).
dftu_scalar mode(const Series& v);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_REDUCE_H
