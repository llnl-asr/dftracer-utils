// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_FIELD_STAT_SIMD_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_FIELD_STAT_SIMD_H

#include <cstdint>

namespace dftracer::utils::dataframe {

// Target-independent reduction result filled by the per-target Highway kernels
// in kernels/field_stat.cpp. Include-guarded so foreach_target's repeated
// inclusion of the translation unit does not redefine it.
struct FsRaw {
    // sum, min, max, then the running mean as the pair shift + cmean and the
    // central moments about it.
    double sum, min, max, shift, cmean, cm2, cm3, cm4;
    // esum wraps; ehigh is the sum of each value's high 32 bits.
    std::int64_t esum, emin, emax, ehigh;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_FIELD_STAT_SIMD_H
