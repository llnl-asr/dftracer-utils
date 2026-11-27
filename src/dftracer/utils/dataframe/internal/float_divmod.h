// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_FLOAT_DIVMOD_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_FLOAT_DIVMOD_H

#include <cmath>

namespace dftracer::utils::dataframe {

/// Python's float `a // b` for a non-zero `b`: the quotient of `a - fmod(a,
/// b)`, so `1.0 // 0.1` is 9.0 (as Python and numpy give), not the 10.0 that
/// `floor(a / b)` rounds to.
// Adapted from CPython's float_divmod (Objects/floatobject.c),
// https://github.com/python/cpython, PSF License Version 2.
inline double float_floordiv(double a, double b) {
    double mod = std::fmod(a, b);
    double div = (a - mod) / b;
    if (mod != 0.0 && ((b < 0) != (mod < 0))) div -= 1.0;
    if (div == 0.0) return std::copysign(0.0, a / b);
    double floordiv = std::floor(div);
    if (div - floordiv > 0.5) floordiv += 1.0;
    return floordiv;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_FLOAT_DIVMOD_H
