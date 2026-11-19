// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_JOIN_NAMES_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_JOIN_NAMES_H

#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/shared_string.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>

namespace dftracer::utils::dataframe {

/// The names separated by ", ", for error and plan text.
template <class Name>
inline dstd::String join_names_of(dstd::Span<const Name> names) {
    dstd::String out;
    dstd::fmt_append_join<", ">(
        out, names,
        [](dstd::String& o, const Name& name) { o += dstd::StringView{name}; });
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
