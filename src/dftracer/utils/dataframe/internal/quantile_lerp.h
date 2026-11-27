// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_QUANTILE_LERP_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_QUANTILE_LERP_H

namespace dftracer::utils::dataframe {

/// The linear quantile between the ranks `a` and `b` at `t` in [0, 1],
/// computed from the nearer end as numpy's `_lerp` is, so `[1, inf]` at 0.95
/// is `inf - inf`, NaN, and at 0 is `1 + inf * 0`, NaN, as numpy and pandas
/// give. A NaN `b` at a whole rank is the NaN cells ranked above every
/// number, and leaves `a`.
inline double quantile_lerp(double a, double b, double t) {
    if (t == 0.0 && b != b) return a;
    const double diff = b - a;
    return t >= 0.5 ? b - diff * (1.0 - t) : a + diff * t;
}

/// pandas' group quantile between the ranks `a` and `b` at `t` in [0, 1]:
/// `a` at a whole rank, `(a + b) / 2` for midpoint, else `a + (b - a) * t`.
inline double group_quantile_lerp(double a, double b, double t, bool midpoint) {
    if (t == 0.0) return a;
    return midpoint ? (a + b) / 2.0 : a + (b - a) * t;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_QUANTILE_LERP_H
