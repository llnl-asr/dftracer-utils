// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_COMPARISON_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_COMPARISON_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/scalar.h>

namespace dftracer::utils::dataframe {

/// Compare each numeric value of `v` against `value`, returning a Bool column
/// (bit-packed) that carries v's validity. `value` may be any numeric type
/// (e.g. gt(col, 100) or gt(col, 1.5)); it is converted to the column's element
/// type, exactly for 64-bit integers. Invalid (empty) for a non-numeric or
/// non-FLAT input.
template <class T>
inline Series gt(const Series& v, T value) {
    return Series{
        dftu_series_compare(v.handle(), DFTU_CMP_GT, to_scalar(value))};
}
template <class T>
inline Series ge(const Series& v, T value) {
    return Series{
        dftu_series_compare(v.handle(), DFTU_CMP_GE, to_scalar(value))};
}
template <class T>
inline Series lt(const Series& v, T value) {
    return Series{
        dftu_series_compare(v.handle(), DFTU_CMP_LT, to_scalar(value))};
}
template <class T>
inline Series le(const Series& v, T value) {
    return Series{
        dftu_series_compare(v.handle(), DFTU_CMP_LE, to_scalar(value))};
}
template <class T>
inline Series eq(const Series& v, T value) {
    return Series{
        dftu_series_compare(v.handle(), DFTU_CMP_EQ, to_scalar(value))};
}
template <class T>
inline Series ne(const Series& v, T value) {
    return Series{
        dftu_series_compare(v.handle(), DFTU_CMP_NE, to_scalar(value))};
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_COMPARISON_H
