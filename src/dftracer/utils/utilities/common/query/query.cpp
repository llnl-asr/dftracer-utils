#include <dftracer/utils/utilities/common/query/query.h>

namespace dftracer::utils::utilities::common::query {

Query::Query(const Query& other) : source_(other.source_) {
    auto result = parse(source_);
    root_ = std::move(*result);
}

Query& Query::operator=(const Query& other) {
    if (this != &other) {
        source_ = other.source_;
        auto result = parse(source_);
        root_ = std::move(*result);
    }
    return *this;
}

dftracer::utils::expected<Query, QueryError> Query::from_string(
    std::string_view input) {
    auto result = parse(input);
    if (!result) return dftracer::utils::unexpected(result.error());
    return Query(std::move(*result), std::string(input));
}

bool Query::evaluate(const json::JsonValue& event) const {
    return query::evaluate(*root_, event);
}

std::string Query::to_string() const { return query::to_string(*root_); }

Query parse_or_throw(std::string_view input) {
    auto result = Query::from_string(input);
    if (!result) throw QueryParseError(result.error());
    return std::move(*result);
}

std::optional<Query> try_parse(std::string_view input) {
    auto result = Query::from_string(input);
    if (!result) return std::nullopt;
    return std::move(*result);
}

}  // namespace dftracer::utils::utilities::common::query
