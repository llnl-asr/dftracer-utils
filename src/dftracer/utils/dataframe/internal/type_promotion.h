#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_TYPE_PROMOTION_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_TYPE_PROMOTION_H

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>

namespace dftracer::utils::dataframe {

/// Applies the one arithmetic/reduction promotion every such kernel entry
/// point runs before dispatch (see is_arithmetic_type's documentation):
/// Float16 -> Float32 (exact) and Decimal128/Decimal256 -> Float64 (lossy).
/// Every other type, including the plain numeric ones, passes through
/// unchanged. Returns `v` itself when no promotion applies; otherwise returns
/// a new column via `owned`, which the caller must dftu_series_free.
inline const dftu_series* promote_for_arithmetic(const dftu_series* v,
                                                 dftu_series*& owned) {
    owned = nullptr;
    if (v->type == TypeId::Float16) {
        owned = dftu_series_cast(v, DFTU_TYPE_FLOAT32);
    } else if (v->type == TypeId::Decimal128 || v->type == TypeId::Decimal256) {
        owned = dftu_series_cast(v, DFTU_TYPE_FLOAT64);
    }
    return owned != nullptr ? owned : v;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_TYPE_PROMOTION_H
