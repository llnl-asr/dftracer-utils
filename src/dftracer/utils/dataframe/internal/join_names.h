// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_JOIN_NAMES_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_JOIN_NAMES_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>

namespace dftracer::utils::dataframe {

/// The names separated by ", ", for error and plan text.
inline dstd::String join_names(const dstd::Vector<dstd::String>& names) {
    dstd::String out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) out += ", ";
        out += names[i];
    }
    return out;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_JOIN_NAMES_H
