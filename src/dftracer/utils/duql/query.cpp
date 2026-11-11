// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>

namespace dftracer::utils::duql {

Query::Query(detail::QueryNodePtr root, dstd::String source)
    : root_(std::move(root)),
      source_(std::move(source)),
      fields_(collect_fields(*root_)),
      any_paths_(collect_any_paths(*root_)),
      has_expressions_(has_expression_leaf(*root_)) {}

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
    return detail::QueryAccess::from_node(detail::make_node(
        detail::AndNode{clone(detail::QueryAccess::root(a)),
                        clone(detail::QueryAccess::root(b))}));
}

dftracer::utils::dstd::Expected<Query, DuqlError> Query::from_string(
    dstd::StringView input) {
    auto result = parse(input);
    if (!result) return dftracer::utils::dstd::Unexpected(result.error());
    return Query(std::move(*result), dstd::String(input));
}

Query detail::QueryAccess::from_node(QueryNodePtr root) {
    dstd::String source = to_string(*root);
    return Query(std::move(root), std::move(source));
}

bool Query::evaluate(const ValueMap& fields) const {
    return duql::evaluate(*root_, fields);
}

bool Query::matches(const json::Value& event) const {
    json::detail::EventView view;
    if (!json::detail::ValueAccess::event(event, view)) return false;
    const OnDemandEvent line(*view.doc, view.text, *view.strings);
    const bool kept = duql::evaluate(*root_, line);
    if (line.broken) json::detail::ValueAccess::mark_broken(event);
    return kept;
}

dstd::String Query::to_string() const { return detail::to_string(*root_); }

dstd::Optional<Query> try_parse(dstd::StringView input) {
    auto result = Query::from_string(input);
    if (!result) return dstd::nullopt;
    return std::move(*result);
}

}  // namespace dftracer::utils::duql
