// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_FIELDS_H
#define DFTRACER_UTILS_DUQL_FIELDS_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/detail/ast.h>

namespace dftracer::utils::duql {

/// Collect all field names referenced in a query AST. Kept out of ast.h so the
/// AST header stays free of the vendored ankerl hash that its return type
/// needs.
dftracer::utils::dstd::StringViewSet collect_fields(
    const detail::QueryNode& node);

/// The paths of the query's any() fields, sorted and unique.
dstd::Vector<dstd::String> collect_any_paths(const detail::QueryNode& node);

/// Whether some leaf under `node` is an @ref ExprLeaf.
bool has_expression_leaf(const detail::QueryNode& node);

/// A copy of the tree under `node`; expression leaves share their terms.
detail::QueryNodePtr clone(const detail::QueryNode& node);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_FIELDS_H
