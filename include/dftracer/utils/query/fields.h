#ifndef DFTRACER_UTILS_QUERY_FIELDS_H
#define DFTRACER_UTILS_QUERY_FIELDS_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/query/ast.h>

#include <string>
#include <vector>

namespace dftracer::utils::query {

/// Collect all field names referenced in a query AST. Kept out of ast.h so the
/// AST header stays free of the vendored ankerl hash that its return type
/// needs.
dftracer::utils::StringViewSet collect_fields(const QueryNode& node);

/// The paths of the query's any() fields, sorted and unique.
std::vector<std::string> collect_any_paths(const QueryNode& node);

}  // namespace dftracer::utils::query

#endif  // DFTRACER_UTILS_QUERY_FIELDS_H
