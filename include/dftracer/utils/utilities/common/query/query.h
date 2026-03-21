#ifndef DFTRACER_UTILS_UTILITIES_COMMON_QUERY_QUERY_H
#define DFTRACER_UTILS_UTILITIES_COMMON_QUERY_QUERY_H

#include <dftracer/utils/utilities/common/query/ast.h>
#include <dftracer/utils/utilities/common/query/evaluator.h>
#include <dftracer/utils/utilities/common/query/parser.h>

#include <optional>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::common::query {

/// Owns a parsed query AST and provides evaluation against JSON.
class Query {
   public:
    static dftracer::utils::expected<Query, QueryError> from_string(
        std::string_view input);

    Query(const Query& other);
    Query& operator=(const Query& other);
    Query(Query&&) = default;
    Query& operator=(Query&&) = default;

    bool evaluate(const json::JsonValue& event) const;
    const QueryNode& root() const { return *root_; }
    const std::string& source() const { return source_; }
    std::string to_string() const;

   private:
    Query(QueryNodePtr root, std::string source)
        : root_(std::move(root)), source_(std::move(source)) {}

    QueryNodePtr root_;
    std::string source_;
};

/// Parse a query string, throwing QueryParseError on failure.
Query parse_or_throw(std::string_view input);

/// Parse a query string into std::optional (nullopt on failure).
std::optional<Query> try_parse(std::string_view input);

}  // namespace dftracer::utils::utilities::common::query

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_QUERY_QUERY_H
