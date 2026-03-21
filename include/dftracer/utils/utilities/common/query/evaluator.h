#ifndef DFTRACER_UTILS_UTILITIES_COMMON_QUERY_EVALUATOR_H
#define DFTRACER_UTILS_UTILITIES_COMMON_QUERY_EVALUATOR_H

#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/query/ast.h>

namespace dftracer::utils::utilities::common::query {

using json::JsonValue;

/// Missing fields and type mismatches evaluate to false.
bool evaluate(const QueryNode& node, const JsonValue& event);

}  // namespace dftracer::utils::utilities::common::query

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_QUERY_EVALUATOR_H
