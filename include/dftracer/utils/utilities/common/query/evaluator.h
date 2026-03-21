#ifndef DFTRACER_UTILS_UTILITIES_COMMON_QUERY_EVALUATOR_H
#define DFTRACER_UTILS_UTILITIES_COMMON_QUERY_EVALUATOR_H

#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/query/ast.h>

#include <string>
#include <unordered_map>

namespace dftracer::utils::utilities::common::query {

using json::JsonValue;

/// Missing fields and type mismatches evaluate to false.
bool evaluate(const QueryNode& node, const JsonValue& event);

using ValueMap = std::unordered_map<std::string, LiteralValue>;

/// Evaluate against a typed key-value map.
/// Missing fields evaluate to false.
bool evaluate(const QueryNode& node, const ValueMap& fields);

}  // namespace dftracer::utils::utilities::common::query

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_QUERY_EVALUATOR_H
