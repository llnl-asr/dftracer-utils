#ifndef DFTRACER_UTILS_QUERY_QUERY_H
#define DFTRACER_UTILS_QUERY_QUERY_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/query/ast.h>
#include <dftracer/utils/query/builder.h>
#include <dftracer/utils/query/evaluator.h>
#include <dftracer/utils/query/fields.h>
#include <dftracer/utils/query/parser.h>

#include <optional>
#include <string>
#include <string_view>

namespace dftracer::utils::query {

/// Owns a parsed query AST and provides evaluation.
class Query {
   public:
    /// Parse a query DSL string. Returns error on invalid syntax.
    static dftracer::utils::expected<Query, QueryError> from_string(
        std::string_view input);

    Query(const Query& other);
    Query& operator=(const Query& other);
    Query(Query&&) = default;
    Query& operator=(Query&&) = default;

    /// Evaluate against a JSON event. Missing fields -> false.
    bool evaluate(const json::JsonValue& event) const;
    /// Evaluate against a typed key-value map. Missing fields -> false.
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

   private:
    Query(QueryNodePtr root, std::string source)
        : root_(std::move(root)),
          source_(std::move(source)),
          fields_(collect_fields(*root_)),
          any_paths_(collect_any_paths(*root_)) {}

    QueryNodePtr root_;
    std::string source_;
    dftracer::utils::StringViewSet fields_;
    std::vector<std::string> any_paths_;
};

inline auto Expr::build() const { return Query::from_string(to_string()); }

/// Parse a query string, throwing QueryParseError on failure.
Query parse_or_throw(std::string_view input);

/// Parse a query string into std::optional (nullopt on failure).
std::optional<Query> try_parse(std::string_view input);

}  // namespace dftracer::utils::query

#endif  // DFTRACER_UTILS_QUERY_QUERY_H
