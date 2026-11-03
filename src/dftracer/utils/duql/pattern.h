// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_PATTERN_H
#define DFTRACER_UTILS_DUQL_PATTERN_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/duql/ast.h>

#include <cstddef>

namespace dftracer::utils::duql {

/// Compiled matcher backing a @ref MatchNode; defined inside the library.
struct CompiledPattern;

struct InSet {
    dftracer::utils::dstd::StringViewSet values;
};

/// Lists at least this long get an @ref InSet.
inline constexpr std::size_t IN_SET_MIN = 16;

/// The @ref InSet of `values`: null below IN_SET_MIN or when a value is not a
/// string. Rebuild it after changing the elements.
dstd::SharedPtr<const InSet> make_in_set(const ArrayNode& values);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_PATTERN_H
