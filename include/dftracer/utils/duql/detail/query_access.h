// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_DETAIL_QUERY_ACCESS_H
#define DFTRACER_UTILS_DUQL_DETAIL_QUERY_ACCESS_H

#include <dftracer/utils/duql/detail/ast.h>

namespace dftracer::utils::duql {

class Expr;
class Query;

namespace detail {

/// The engine's door to the AST inside @ref Expr and @ref Query. The Expr
/// members are defined in builder.h and the Query members in query.h, so
/// include the header of the class you reach.
struct QueryAccess {
    /// An @ref Expr that owns `node`.
    static Expr expr(QueryNodePtr node);
    /// The node of `e`, which stays valid while `e` lives.
    static const QueryNode& node(const Expr& e);
    /// The node of `e`; `e` is empty afterwards.
    static QueryNodePtr release(Expr& e);
    /// A @ref Query over `root`, whose source is the filter's text.
    static Query from_node(QueryNodePtr root);
    /// The root node of `q`, which stays valid while `q` lives.
    static const QueryNode& root(const Query& q);
};

}  // namespace detail

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_DETAIL_QUERY_ACCESS_H
