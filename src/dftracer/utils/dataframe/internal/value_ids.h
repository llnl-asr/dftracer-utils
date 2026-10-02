#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_VALUE_IDS_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_VALUE_IDS_H

#include <dftracer/utils/dataframe/internal/column_data.h>

#include <cstdint>
#include <vector>

namespace dftracer::utils::dataframe {

/// Distinct-value ids of a column: `ids[i]` is the id of row `i`, assigned in
/// first-seen row order, and `first[id]` is the first row holding it.
struct ValueIds {
    std::vector<std::int32_t> ids;
    std::vector<std::int64_t> first;
    /// The id of the null rows, -1 when there are none.
    std::int32_t null_id = -1;
};

/// Fills `out` for a column in any layout, comparing rows as the dedupe keys
/// of append_cell do: a null is one value, byte-domain values compare by their
/// bytes, Bool by its bit, other fixed-width values by their raw bytes.
/// Returns false, leaving `out` untouched, for a type with no per-row value.
bool value_ids(const dftu_series& v, ValueIds& out);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_VALUE_IDS_H
