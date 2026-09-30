#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_VARWIDTH_OFFSETS_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_VARWIDTH_OFFSETS_H

#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/types.h>

#include <cstdint>
#include <memory>

// String/Binary/List columns carry int32 offsets and their Large* variants
// int64 ones, in the one `offsets` buffer. A kernel's hot loop is written once
// as a template on the width `Off`, and the caller dispatches once, at the
// entry point, on the column's type - not per row.
namespace dftracer::utils::dataframe {

/// The offsets buffer of `c`, read at width `Off`.
template <class Off>
inline const std::shared_ptr<Buffer>& offsets_of(const dftu_series& c) {
    return c.offsets;
}
template <class Off>
inline std::shared_ptr<Buffer>& offsets_of(dftu_series& c) {
    return c.offsets;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_VARWIDTH_OFFSETS_H
