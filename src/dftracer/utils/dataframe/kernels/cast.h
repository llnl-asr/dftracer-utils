#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_CAST_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_CAST_H

#include <dftracer/utils/dataframe/dataframe.h>

namespace dftracer::utils::dataframe {

/// Cast a FLAT column to `target`: numeric to numeric, integer / float / Bool
/// to String, integer / float to Bool, a non-numeric column to its own type. A
/// new FLAT column carrying the source validity. Invalid (empty) for any other
/// pair.
Series cast(const Series& v, TypeId target);

/// SIMD fast path for the numeric casts Highway maps onto one ConvertTo /
/// Promote / Demote (float <-> same-width int, float widen/narrow). Writes `n`
/// converted values to `dv` and returns true; returns false for pairs it does
/// not vectorize, leaving the scalar path to handle them.
bool cast_simd(std::int32_t src, std::int32_t dst, const void* sv, void* dv,
               std::size_t n);

/// SIMD number -> Bool for a column with no nulls: writes the packed result
/// bits to `bits` (zeroed by the caller) and, for a float, a validity bit per
/// row to `valid` (also zeroed; a NaN row has none). Returns the number of null
/// (NaN) rows, or -1 for a source type it does not vectorize (Int32, Int64,
/// Float32, Float64 are handled), leaving the scalar path to the caller.
std::int64_t cast_bool_simd(std::int32_t src, const void* sv,
                            std::uint8_t* bits, std::uint8_t* valid,
                            std::size_t n);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_CAST_H
