#ifndef DFTRACER_UTILS_DUQL_QUERY_H
#define DFTRACER_UTILS_DUQL_QUERY_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/duql/ast.h>
#include <dftracer/utils/duql/builder.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/parser.h>

#include <optional>
#include <string>
#include <string_view>

namespace dftracer::utils::duql {

/// Owns a parsed query AST and provides evaluation.
class Query {
   public:
    /// Parse a query DSL string. Returns error on invalid syntax.
    static dftracer::utils::expected<Query, DuqlError> from_string(
        std::string_view input);
    /// A query over a compiled filter; its source is the filter's text.
    static Query from_node(QueryNodePtr root);

    Query(const Query& other);
    Query& operator=(const Query& other);
    Query(Query&&) = default;
    Query& operator=(Query&&) = default;

    /// Whether the filter keeps `event`: TRUE under three-valued logic, so a
    /// leaf on a missing or null field, or across types, does not pass.
    /// With `args_fallback`, a bare name the event lacks reads the field of
    /// that name under `args`.
    bool evaluate(const json::JsonValue& event,
                  bool args_fallback = true) const;
    /// As above, on a flattened record (a key absent from `fields` is
    /// missing).
    bool evaluate(const ValueMap& fields) const;
    /// Access the root AST node.
    const QueryNode& root() const { return *root_; }
    /// Original query source string.
    const std::string& source() const { return source_; }
    /// Serialize AST back to query DSL string.
    std::string to_string() const;
    /// Fields referenced by this query, precomputed at construction.
    const dftracer::utils::StringViewSet& fields() const { return fields_; }
    bool references(std::string_view field) const {
        return fields_.count(field) > 0;
    }
    /// The paths used as any(path), sorted.
    const std::vector<std::string>& any_paths() const { return any_paths_; }
    /// Whether some leaf is an expression (ExprLeaf), which may read a whole
    /// array or object.
    bool has_expressions() const { return has_expressions_; }

   private:
    Query(QueryNodePtr root, std::string source)
        : root_(std::move(root)),
          source_(std::move(source)),
          fields_(collect_fields(*root_)),
          any_paths_(collect_any_paths(*root_)),
          has_expressions_(has_expression_leaf(*root_)) {}

    QueryNodePtr root_;
    std::string source_;
    dftracer::utils::StringViewSet fields_;
    std::vector<std::string> any_paths_;
    bool has_expressions_ = false;
};

/// `a and b`, sharing the expression terms of both.
Query conjoin(const Query& a, const Query& b);

inline auto Expr::build() const { return Query::from_string(to_string()); }

/// Parse a query string, throwing DuqlParseError on failure.
Query parse_or_throw(std::string_view input);

/// Parse a query string into std::optional (nullopt on failure).
std::optional<Query> try_parse(std::string_view input);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_QUERY_H
