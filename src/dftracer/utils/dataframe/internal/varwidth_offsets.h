#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_VARWIDTH_OFFSETS_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_VARWIDTH_OFFSETS_H

#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/types.h>

#include <cstdint>
#include <memory>

// String/Binary/List columns carry their offsets in one of two physical
// layouts: 32-bit (dftu_series::offsets, String/Binary/List) or 64-bit
// (dftu_series::offsets64, LargeString/LargeBinary/LargeList) - never both.
// OffsetTraits<Off> picks the right buffer member for a given offset width at
// compile time, so a kernel's hot loop is written once as a template on `Off`
// and the caller dispatches ONCE, at the entry point, on which buffer the
// column actually carries - not per row.
namespace dftracer::utils::dataframe {

template <class Off>
struct OffsetTraits;

template <>
struct OffsetTraits<std::int32_t> {
    static constexpr std::shared_ptr<Buffer> dftu_series::* member =
        &dftu_series::offsets;
};

template <>
struct OffsetTraits<std::int64_t> {
    static constexpr std::shared_ptr<Buffer> dftu_series::* member =
        &dftu_series::offsets64;
};

/// The offsets buffer of `c` at width `Off` (c.offsets for int32_t,
/// c.offsets64 for int64_t), regardless of `c.type`.
template <class Off>
inline const std::shared_ptr<Buffer>& offsets_of(const dftu_series& c) {
    return c.*OffsetTraits<Off>::member;
}
template <class Off>
inline std::shared_ptr<Buffer>& offsets_of(dftu_series& c) {
    return c.*OffsetTraits<Off>::member;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_VARWIDTH_OFFSETS_H
