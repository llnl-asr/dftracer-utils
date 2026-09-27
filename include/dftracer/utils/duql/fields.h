#ifndef DFTRACER_UTILS_DUQL_FIELDS_H
#define DFTRACER_UTILS_DUQL_FIELDS_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/duql/ast.h>

#include <string>
#include <vector>

namespace dftracer::utils::duql {

/// Collect all field names referenced in a query AST. Kept out of ast.h so the
/// AST header stays free of the vendored ankerl hash that its return type
/// needs.
dftracer::utils::StringViewSet collect_fields(const QueryNode& node);

/// The paths of the query's any() fields, sorted and unique.
std::vector<std::string> collect_any_paths(const QueryNode& node);

/// Whether some leaf under `node` is an ExprLeaf.
bool has_expression_leaf(const QueryNode& node);

/// A copy of the tree under `node`; expression leaves share their terms.
QueryNodePtr clone(const QueryNode& node);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_FIELDS_H
