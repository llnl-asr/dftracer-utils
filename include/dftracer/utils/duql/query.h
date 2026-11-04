// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_QUERY_H
#define DFTRACER_UTILS_DUQL_QUERY_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/builder.h>
#include <dftracer/utils/duql/detail/ast.h>
#include <dftracer/utils/duql/detail/query_access.h>
#include <dftracer/utils/duql/error.h>
#include <dftracer/utils/duql/value_map.h>

namespace dftracer::utils::duql {

/// Owns a parsed query AST and provides evaluation.
class Query {
   public:
    /// Parse a query DSL string. Returns error on invalid syntax.
    static dftracer::utils::dstd::Expected<Query, DuqlError> from_string(
        dstd::StringView input);

    Query(const Query& other);
    Query& operator=(const Query& other);
    Query(Query&&) = default;
    Query& operator=(Query&&) = default;

    /// Whether the filter keeps the flattened record `fields`: TRUE under
    /// three-valued logic, so a leaf on a missing or null field, or across
    /// types, does not pass. A key absent from `fields` is missing.
    bool evaluate(const ValueMap& fields) const;
    /// Original query source string.
    const dstd::String& source() const { return source_; }
    /// Serialize AST back to query DSL string.
    dstd::String to_string() const;
    /// Fields referenced by this query, precomputed at construction.
    const dftracer::utils::dstd::StringViewSet& fields() const {
        return fields_;
    }
    bool references(dstd::StringView field) const {
        return fields_.count(field) > 0;
    }
    /// The paths used as any(path), sorted.
    const dstd::Vector<dstd::String>& any_paths() const { return any_paths_; }
    /// Whether some leaf is an expression (arithmetic, a function, field
    /// against field), which may read a whole array or object.
    bool has_expressions() const { return has_expressions_; }

   private:
    friend struct detail::QueryAccess;

    Query(detail::QueryNodePtr root, dstd::String source);

    detail::QueryNodePtr root_;
    dstd::String source_;
    dftracer::utils::dstd::StringViewSet fields_;
    dstd::Vector<dstd::String> any_paths_;
    bool has_expressions_ = false;
};

namespace detail {

inline const QueryNode& QueryAccess::root(const Query& q) { return *q.root_; }

}  // namespace detail

/// `a and b`, sharing the expression terms of both.
Query conjoin(const Query& a, const Query& b);

inline auto Expr::build() const { return Query::from_string(to_string()); }

/// Parse a query string, throwing @ref DuqlParseError on failure.
Query parse_or_throw(dstd::StringView input);

/// Parse a query string into dstd::Optional (nullopt on failure).
dstd::Optional<Query> try_parse(dstd::StringView input);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_QUERY_H
