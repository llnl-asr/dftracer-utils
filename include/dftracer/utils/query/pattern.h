#ifndef DFTRACER_UTILS_QUERY_PATTERN_H
#define DFTRACER_UTILS_QUERY_PATTERN_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/query/ast.h>

#include <cstddef>
#include <memory>
#include <regex>

namespace dftracer::utils::query {

/// Compiled matcher backing a MatchNode.
struct CompiledPattern {
    std::regex re;
};

struct InSet {
    dftracer::utils::StringViewSet values;
};

/// Lists at least this long get an InSet.
inline constexpr std::size_t IN_SET_MIN = 16;

/// The InSet of `values`: null below IN_SET_MIN or when a value is not a
/// string. Rebuild it after changing the elements.
std::shared_ptr<const InSet> make_in_set(const ArrayNode& values);

}  // namespace dftracer::utils::query

#endif  // DFTRACER_UTILS_QUERY_PATTERN_H
