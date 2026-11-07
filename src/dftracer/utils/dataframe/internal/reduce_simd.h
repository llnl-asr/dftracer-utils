// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_REDUCE_SIMD_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_REDUCE_SIMD_H

#include <dftracer/utils/dataframe/abi.h>

#include <cstdint>

struct dftu_series;

namespace dftracer::utils::dataframe {

/// Reduce `v` (DFTU_REDUCE_SUM/MIN/MAX) into `out` with a vectorized kernel:
/// a multi-accumulator sum for Float64 (the serial fadd chain the compiler will
/// not reassociate) and a lane-wise Min/Max for the numeric types. Applies only
/// when `v` has no nulls and is non-empty; returns true if handled, false to
/// fall back to the scalar path. The SIMD sum may differ from the sequential
/// sum in the last ULP because the summation order changes.
bool reduce(const dftu_series& v, std::int32_t op, dftu_scalar& out);

/// Null-aware SIMD scan of a bool bitmap: `any_bits` returns true if any valid
/// bit is set; `all_bits` returns true if every valid bit is set (both true for
/// an all-null or empty column). Result is identical to the scalar bit scan.
bool any_bits(const dftu_series& v);
bool all_bits(const dftu_series& v);

/// Vectorized product of a null-free i64/u64/f64 column into `out`; returns
/// false (leaving `out` untouched) for columns with validity or a narrower type
/// whose product accumulates in a wider domain, which stay on the scalar path.
/// Integer results match the scalar wrapping product exactly; the f64 result
/// may differ from a strict left fold in the last ULP due to reassociation.
bool product_simd(const dftu_series& v, dftu_scalar& out);

/// SIMD arg_min / arg_max over a FLAT, no-null numeric column: writes the
/// earliest index of the extreme value to `out_idx` and returns true. Returns
/// false (leaving the scalar path to handle it) for null-bearing or
/// non-fixed-width columns.
bool arg_extreme_simd(const dftu_series& v, bool is_min, std::int64_t& out_idx);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_REDUCE_SIMD_H
