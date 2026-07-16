#include <dftracer/utils/utilities/common/query/evaluator.h>
#include <dftracer/utils/utilities/common/query/pattern.h>

#include <cmath>
#include <regex>
#include <string>
#include <string_view>
#include <variant>

namespace dftracer::utils::utilities::common::query {

namespace {

// Returns -1/0/1 for less/equal/greater, nullopt on type mismatch.
std::optional<int> compare_value(const JsonValue& field,
                                 const LiteralNode& lit) {
    return std::visit(
        [&field](auto&& v) -> std::optional<int> {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                if (!field.is_string()) return std::nullopt;
                auto fv = field.get<std::string_view>();
                std::string_view sv(v);
                if (fv < sv) return -1;
                if (fv > sv) return 1;
                return 0;
            } else if constexpr (std::is_same_v<T, int64_t>) {
                if (field.is_int()) {
                    auto fv = field.get<int64_t>();
                    if (fv < v) return -1;
                    if (fv > v) return 1;
                    return 0;
                }
                if (field.is_uint()) {
                    auto fv = field.get<uint64_t>();
                    if (v < 0) return 1;
                    auto uv = static_cast<uint64_t>(v);
                    if (fv < uv) return -1;
                    if (fv > uv) return 1;
                    return 0;
                }
                if (field.is_number()) {
                    auto fv = field.get<double>();
                    auto dv = static_cast<double>(v);
                    if (fv < dv) return -1;
                    if (fv > dv) return 1;
                    return 0;
                }
                return std::nullopt;
            } else if constexpr (std::is_same_v<T, uint64_t>) {
                if (field.is_uint()) {
                    auto fv = field.get<uint64_t>();
                    if (fv < v) return -1;
                    if (fv > v) return 1;
                    return 0;
                }
                if (field.is_int()) {
                    auto fv = field.get<int64_t>();
                    if (fv < 0) return -1;
                    auto ufv = static_cast<uint64_t>(fv);
                    if (ufv < v) return -1;
                    if (ufv > v) return 1;
                    return 0;
                }
                if (field.is_number()) {
                    auto fv = field.get<double>();
                    auto dv = static_cast<double>(v);
                    if (fv < dv) return -1;
                    if (fv > dv) return 1;
                    return 0;
                }
                return std::nullopt;
            } else if constexpr (std::is_same_v<T, double>) {
                if (!field.is_number()) return std::nullopt;
                auto fv = field.get<double>();
                if (fv < v) return -1;
                if (fv > v) return 1;
                return 0;
            } else if constexpr (std::is_same_v<T, bool>) {
                if (!field.is_bool()) return std::nullopt;
                auto fv = field.get<bool>();
                if (fv == v) return 0;
                return fv ? 1 : -1;
            } else {
                return std::nullopt;
            }
        },
        lit.value);
}

bool apply_compare(CompareOp op, std::optional<int> cmp) {
    if (!cmp) return false;
    switch (op) {
        case CompareOp::EQ:
            return *cmp == 0;
        case CompareOp::NE:
            return *cmp != 0;
        case CompareOp::GT:
            return *cmp > 0;
        case CompareOp::LT:
            return *cmp < 0;
        case CompareOp::GE:
            return *cmp >= 0;
        case CompareOp::LE:
            return *cmp <= 0;
    }
    return false;
}

JsonValue resolve_field(const JsonValue& event, const FieldNode& field) {
    auto v = event.at(field.path);
    if (!v.is_null()) return v;
    // DFTracer nests domain fields (fhash, hhash, ret, level, ...) under
    // "args". Let a bare reference resolve there so nested fields are queryable
    // by bare name, matching the index dimension names and the ValueMap path.
    if (field.path.find('.') == std::string::npos) {
        return event.at("args." + field.path);
    }
    return v;
}

bool eval_node(const QueryNode& node, const JsonValue& event);

bool eval_compare(const CompareNode& n, const JsonValue& event) {
    auto fv = resolve_field(event, n.field);
    if (fv.is_null()) return false;
    return apply_compare(n.op, compare_value(fv, n.value));
}

bool eval_in(const InNode& n, const JsonValue& event) {
    auto fv = resolve_field(event, n.field);
    if (fv.is_null()) return false;
    for (auto& elem : n.values.elements) {
        auto cmp = compare_value(fv, elem);
        if (cmp && *cmp == 0) return true;
    }
    return false;
}

bool eval_not_in(const NotInNode& n, const JsonValue& event) {
    auto fv = resolve_field(event, n.field);
    if (fv.is_null()) return false;
    for (auto& elem : n.values.elements) {
        auto cmp = compare_value(fv, elem);
        if (cmp && *cmp == 0) return false;
    }
    return true;
}

bool eval_match(const MatchNode& n, const JsonValue& event) {
    if (!n.compiled) return false;
    auto fv = resolve_field(event, n.field);
    if (!fv.is_string()) return false;
    auto sv = fv.get<std::string_view>();
    bool m = std::regex_search(sv.begin(), sv.end(), n.compiled->re);
    return n.negated ? !m : m;
}

bool eval_node(const QueryNode& node, const JsonValue& event) {
    return std::visit(
        [&event](auto&& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, CompareNode>) {
                return eval_compare(n, event);
            } else if constexpr (std::is_same_v<T, InNode>) {
                return eval_in(n, event);
            } else if constexpr (std::is_same_v<T, NotInNode>) {
                return eval_not_in(n, event);
            } else if constexpr (std::is_same_v<T, MatchNode>) {
                return eval_match(n, event);
            } else if constexpr (std::is_same_v<T, AndNode>) {
                return eval_node(*n.left, event) && eval_node(*n.right, event);
            } else if constexpr (std::is_same_v<T, OrNode>) {
                return eval_node(*n.left, event) || eval_node(*n.right, event);
            } else if constexpr (std::is_same_v<T, NotNode>) {
                return !eval_node(*n.operand, event);
            } else {
                return false;
            }
        },
        node.data);
}

}  // namespace

bool evaluate(const QueryNode& node, const JsonValue& event) {
    return eval_node(node, event);
}

namespace {

std::optional<int> compare_literals(const LiteralValue& a,
                                    const LiteralValue& b) {
    return std::visit(
        [](auto&& va, auto&& vb) -> std::optional<int> {
            using A = std::decay_t<decltype(va)>;
            using B = std::decay_t<decltype(vb)>;
            if constexpr (std::is_same_v<A, B>) {
                if (va < vb) return -1;
                if (va > vb) return 1;
                return 0;
            } else if constexpr (std::is_arithmetic_v<A> &&
                                 std::is_arithmetic_v<B>) {
                double da = static_cast<double>(va);
                double db = static_cast<double>(vb);
                if (da < db) return -1;
                if (da > db) return 1;
                return 0;
            } else {
                return std::nullopt;
            }
        },
        a, b);
}

bool eval_map_node(const QueryNode& node, const ValueMap& fields);

bool eval_map_node(const QueryNode& node, const ValueMap& fields) {
    return std::visit(
        [&fields](auto&& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, CompareNode>) {
                auto it = fields.find(n.field.path);
                if (it == fields.end()) return false;
                auto cmp = compare_literals(it->second, n.value.value);
                return apply_compare(n.op, cmp);
            } else if constexpr (std::is_same_v<T, InNode>) {
                auto it = fields.find(n.field.path);
                if (it == fields.end()) return false;
                for (const auto& elem : n.values.elements) {
                    auto cmp = compare_literals(it->second, elem.value);
                    if (cmp && *cmp == 0) return true;
                }
                return false;
            } else if constexpr (std::is_same_v<T, NotInNode>) {
                auto it = fields.find(n.field.path);
                if (it == fields.end()) return false;
                for (const auto& elem : n.values.elements) {
                    auto cmp = compare_literals(it->second, elem.value);
                    if (cmp && *cmp == 0) return false;
                }
                return true;
            } else if constexpr (std::is_same_v<T, MatchNode>) {
                if (!n.compiled) return false;
                auto it = fields.find(n.field.path);
                if (it == fields.end()) return false;
                if (!std::holds_alternative<std::string>(it->second))
                    return false;
                const auto& s = std::get<std::string>(it->second);
                bool m = std::regex_search(s.begin(), s.end(), n.compiled->re);
                return n.negated ? !m : m;
            } else if constexpr (std::is_same_v<T, AndNode>) {
                return eval_map_node(*n.left, fields) &&
                       eval_map_node(*n.right, fields);
            } else if constexpr (std::is_same_v<T, OrNode>) {
                return eval_map_node(*n.left, fields) ||
                       eval_map_node(*n.right, fields);
            } else if constexpr (std::is_same_v<T, NotNode>) {
                return !eval_map_node(*n.operand, fields);
            } else {
                return false;
            }
        },
        node.data);
}

}  // namespace

bool evaluate(const QueryNode& node, const ValueMap& fields) {
    return eval_map_node(node, fields);
}

}  // namespace dftracer::utils::utilities::common::query
