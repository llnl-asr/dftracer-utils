#include <dftracer/utils/duql/query.h>

namespace dftracer::utils::duql {

Query::Query(const Query& other)
    : root_(clone(*other.root_)),
      source_(other.source_),
      // Views into this query's own AST, not the other's.
      fields_(collect_fields(*root_)),
      any_paths_(other.any_paths_),
      has_expressions_(other.has_expressions_) {}

Query& Query::operator=(const Query& other) {
    if (this != &other) *this = Query(other);
    return *this;
}

Query conjoin(const Query& a, const Query& b) {
    return Query::from_node(
        make_node(AndNode{clone(a.root()), clone(b.root())}));
}

dftracer::utils::expected<Query, DuqlError> Query::from_string(
    std::string_view input) {
    auto result = parse(input);
    if (!result) return dftracer::utils::unexpected(result.error());
    return Query(std::move(*result), std::string(input));
}

Query Query::from_node(QueryNodePtr root) {
    std::string source = duql::to_string(*root);
    return Query(std::move(root), std::move(source));
}

bool Query::evaluate(const json::JsonValue& event, bool args_fallback) const {
    return duql::evaluate(*root_, event, args_fallback);
}

bool Query::evaluate(const ValueMap& fields) const {
    return duql::evaluate(*root_, fields);
}

std::string Query::to_string() const { return duql::to_string(*root_); }

Query parse_or_throw(std::string_view input) {
    auto result = Query::from_string(input);
    if (!result) throw DuqlParseError(result.error());
    return std::move(*result);
}

std::optional<Query> try_parse(std::string_view input) {
    auto result = Query::from_string(input);
    if (!result) return std::nullopt;
    return std::move(*result);
}

}  // namespace dftracer::utils::duql
