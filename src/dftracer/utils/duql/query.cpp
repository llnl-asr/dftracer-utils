// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
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

dftracer::utils::dstd::Expected<Query, DuqlError> Query::from_string(
    dstd::StringView input) {
    auto result = parse(input);
    if (!result) return dftracer::utils::dstd::Unexpected(result.error());
    return Query(std::move(*result), dstd::String(input));
}

Query Query::from_node(QueryNodePtr root) {
    dstd::String source = duql::to_string(*root);
    return Query(std::move(root), std::move(source));
}

bool Query::evaluate(const json::JsonValue& event, bool args_fallback) const {
    return duql::evaluate(*root_, event, args_fallback);
}

bool Query::evaluate(const ValueMap& fields) const {
    return duql::evaluate(*root_, fields);
}

dstd::String Query::to_string() const { return duql::to_string(*root_); }

Query parse_or_throw(dstd::StringView input) {
    auto result = Query::from_string(input);
    if (!result) throw DuqlParseError(result.error());
    return std::move(*result);
}

dstd::Optional<Query> try_parse(dstd::StringView input) {
    auto result = Query::from_string(input);
    if (!result) return dstd::nullopt;
    return std::move(*result);
}

}  // namespace dftracer::utils::duql
