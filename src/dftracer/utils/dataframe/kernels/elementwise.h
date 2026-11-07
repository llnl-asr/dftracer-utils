// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_ELEMENTWISE_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_ELEMENTWISE_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/scalar.h>

// Elementwise column transforms returning a new FLAT column. abs/clip/round are
// Highway-SIMD; fillna/cumsum are scalar (null-aware / sequential). Validity is
// shared zero-copy where the transform preserves it.
namespace dftracer::utils::dataframe {

/// Absolute value (unsigned columns are copied unchanged).
Series abs(const Series& v);

/// Clamp each value into [lo, hi] (scalars converted to the element type).
Series clip(const Series& v, dftu_scalar lo, dftu_scalar hi);

/// Round floats to the nearest integer value (integers unchanged).
Series round(const Series& v);

/// Replace null values with `fill`; the result has no nulls.
Series fillna(const Series& v, dftu_scalar fill);

/// Cumulative sum along the column (nulls contribute 0), same element type.
Series cumsum(const Series& v);

/// Cumulative (running) maximum along the column, skipping nulls; same type.
Series cummax(const Series& v);

/// Cumulative (running) minimum along the column, skipping nulls; same type.
Series cummin(const Series& v);

/// Cumulative (running) product (nulls contribute 1), same element type.
Series cum_prod(const Series& v);

/// Running count of non-null rows, as an Int64 column.
Series cum_count(const Series& v);

/// Round floats toward +inf / -inf / zero (integers unchanged); SIMD.
Series ceil(const Series& v);
Series floor(const Series& v);
Series trunc(const Series& v);

/// Sign of each value as -1/0/1 in the column's type (SIMD).
Series sign(const Series& v);

/// Unary minus (SIMD).
Series negate(const Series& v);

/// First discrete difference x[i]-x[i-1] (same type); row 0 is null.
Series diff(const Series& v);

/// Percent change (x[i]-x[i-1])/x[i-1] as Float64; row 0 is null.
Series pct_change(const Series& v);

/// sqrt/exp/log over the values, as a Float64 column (sqrt is SIMD).
Series sqrt(const Series& v);
Series exp(const Series& v);
Series log(const Series& v);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_ELEMENTWISE_H
