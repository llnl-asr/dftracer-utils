// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_JOIN_NAMES_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_JOIN_NAMES_H

#include <dftracer/utils/dstd/shared_string.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>

namespace dftracer::utils::dataframe {

/// The names separated by ", ", for error and plan text.
template <class Name>
inline dstd::String join_names_of(dstd::Span<const Name> names) {
    dstd::String out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) out += ", ";
        out += dstd::StringView{names[i]};
    }
    return out;
}

inline dstd::String join_names(dstd::Span<const dstd::String> names) {
    return join_names_of<dstd::String>(names);
}

inline dstd::String join_names(dstd::Span<const dstd::SharedString> names) {
    return join_names_of<dstd::SharedString>(names);
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_JOIN_NAMES_H
