// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_DATAFRAME_SELECTION_OVER_H
#define DFTRACER_UTILS_TESTS_DATAFRAME_SELECTION_OVER_H

#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

// A SELECTION column over `base`: row k of the result is row idx[k] of the
// base, a negative index being a null row.
inline dftracer::utils::dataframe::Series selection_over(
    const dftracer::utils::dataframe::Series& base,
    dftracer::utils::dstd::Span<const std::int64_t> idx) {
    auto* o = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*o, *base.handle());
    o->encoding = dftracer::utils::dataframe::Encoding::Selection;
    o->length = static_cast<std::int64_t>(idx.size());
    o->data = dftracer::utils::dataframe::new_buffer(idx.size() *
                                                     sizeof(std::int64_t));
    if (!idx.empty())
        std::memcpy(dftracer::utils::dataframe::writable(o->data), idx.data(),
                    idx.size() * sizeof(std::int64_t));
    o->set_child(
        dftracer::utils::dstd::make_shared<dftu_series>(*base.handle()));
    return dftracer::utils::dataframe::Series{o};
}

inline dftracer::utils::dataframe::Series selection_over(
    const dftracer::utils::dataframe::Series& base,
    std::initializer_list<std::int64_t> idx) {
    return selection_over(base, dftracer::utils::dstd::Span<const std::int64_t>(
                                    idx.begin(), idx.size()));
}

#endif  // DFTRACER_UTILS_TESTS_DATAFRAME_SELECTION_OVER_H
