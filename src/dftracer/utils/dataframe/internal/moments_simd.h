// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_MOMENTS_SIMD_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_MOMENTS_SIMD_H

#include <cstddef>

// SIMD (Highway) reductions for moment-based statistics over a double array.
namespace dftracer::utils::dataframe {

/// SIMD sum of `x[0..n)`.
double sum_f64(const double* x, std::size_t n);

/// SIMD central moments about `mean`: out = {sum (x-mean)^2, ^3, ^4}.
void central_moments(const double* x, std::size_t n, double mean,
                     double out[3]);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_MOMENTS_SIMD_H
