#ifndef DFTRACER_UTILS_DUQL_PATTERN_H
#define DFTRACER_UTILS_DUQL_PATTERN_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/duql/ast.h>

#include <cstddef>
#include <memory>

namespace dftracer::utils::duql {

/// Compiled matcher backing a MatchNode; defined inside the library.
struct CompiledPattern;

struct InSet {
    dftracer::utils::StringViewSet values;
};

/// Lists at least this long get an InSet.
inline constexpr std::size_t IN_SET_MIN = 16;

/// The InSet of `values`: null below IN_SET_MIN or when a value is not a
/// string. Rebuild it after changing the elements.
std::shared_ptr<const InSet> make_in_set(const ArrayNode& values);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_PATTERN_H
