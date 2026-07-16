#ifndef DFTRACER_UTILS_UTILITIES_COMMON_QUERY_PATTERN_H
#define DFTRACER_UTILS_UTILITIES_COMMON_QUERY_PATTERN_H

#include <regex>

namespace dftracer::utils::utilities::common::query {

/// Compiled matcher backing a MatchNode.
struct CompiledPattern {
    std::regex re;
};

}  // namespace dftracer::utils::utilities::common::query

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_QUERY_PATTERN_H
