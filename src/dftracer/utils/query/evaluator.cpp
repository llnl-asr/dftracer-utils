#include <dftracer/utils/core/common/field_ref.h>
#include <dftracer/utils/json/canonical.h>
#include <dftracer/utils/query/evaluator.h>
#include <dftracer/utils/query/pattern.h>

#include <algorithm>
#include <cmath>
#include <regex>
#include <string>
#include <string_view>
#include <variant>

namespace dftracer::utils::query {

namespace {

// Returns -1/0/1 for less/equal/greater, nullopt on type mismatch.
std::optional<int> compare_value(const JsonValue& field,
                                 const LiteralNode& lit) {
    return std::visit(
        [&field](auto&& v) -> std::optional<int> {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                if (field.is_string()) {
                    auto fv = field.get<std::string_view>();
                    std::string_view sv(v);
                    if (fv < sv) return -1;
                    if (fv > sv) return 1;
                    return 0;
                }
                // An object or array compares as canonical JSON text.
                if (!field.is_object() && !field.is_array())
                    return std::nullopt;
                std::string fv;
                json::append_canonical_json(fv, field.element());
                const std::string sv = json::canonical_json_text(v);
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
    // "args". Any field not already prefixed resolves there too: a bare name,
    // or a flat arg key that itself contains dots (e.g. "cqe.raw_ns"). at()'s
    // flat-key fallback matches the dotted key against the flat args member.
    if (!has_args_prefix(field.path)) return event.at("args." + field.path);
    return v;
}

bool eval_node(const QueryNode& node, const JsonValue& event);

// Whether `holds` is true for the field's value or, for an any() field, for
// a scalar element of the array at its path.
template <class Holds>
bool field_holds(const FieldNode& field, const JsonValue& event,
                 Holds&& holds) {
    const JsonValue fv = resolve_field(event, field);
    if (!field.any) return !fv.is_null() && holds(fv);
    if (!fv.is_array()) return false;
    for (auto el : fv.element().get_array().value_unsafe()) {
        const JsonValue ev(el);
        if (ev.is_null() || ev.is_object() || ev.is_array()) continue;
        if (holds(ev)) return true;
    }
    return false;
}

bool eval_compare(const CompareNode& n, const JsonValue& event) {
    return field_holds(n.field, event, [&](const JsonValue& fv) {
        return apply_compare(n.op, compare_value(fv, n.value));
    });
}

bool in_values(const ArrayNode& values, const JsonValue& fv) {
    if (values.set) {
        if (fv.is_string())
            return values.set->values.contains(fv.get<std::string_view>());
        if (!fv.is_object() && !fv.is_array()) return false;
    }
    for (auto& elem : values.elements) {
        auto cmp = compare_value(fv, elem);
        if (cmp && *cmp == 0) return true;
    }
    return false;
}

bool eval_in(const InNode& n, const JsonValue& event) {
    return field_holds(n.field, event, [&](const JsonValue& fv) {
        return in_values(n.values, fv);
    });
}

bool eval_not_in(const NotInNode& n, const JsonValue& event) {
    return field_holds(n.field, event, [&](const JsonValue& fv) {
        return !in_values(n.values, fv);
    });
}

bool eval_match(const MatchNode& n, const JsonValue& event) {
    if (!n.compiled) return false;
    return field_holds(n.field, event, [&](const JsonValue& fv) {
        if (!fv.is_string()) return false;
        auto sv = fv.get<std::string_view>();
        bool m = std::regex_search(sv.begin(), sv.end(), n.compiled->re);
        return n.negated ? !m : m;
    });
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

// Whether `digits` is a non-empty run of decimal digits.
bool is_position(std::string_view digits) {
    return !digits.empty() &&
           std::all_of(digits.begin(), digits.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
}

// Whether `key` is `<path>.<k>` for a position `k`.
bool is_position_of(std::string_view key, std::string_view path) {
    return key.size() > path.size() + 1 && key.starts_with(path) &&
           key[path.size()] == '.' && is_position(key.substr(path.size() + 1));
}

// Whether `holds` is true for the field's value or, for an any() field, for
// a value at one of its array positions (`<path>.<k>`, or without an "args."
// prefix, as event args are keyed).
template <class Holds>
bool map_holds(const FieldNode& field, const ValueMap& fields, Holds&& holds) {
    if (!field.any) {
        auto it = fields.find(field.path);
        return it != fields.end() && holds(it->second);
    }
    const std::string_view path = field.path;
    const std::string_view bare =
        path.starts_with("args.") ? path.substr(5) : path;
    for (const auto& [key, value] : fields)
        if ((is_position_of(key, path) || is_position_of(key, bare)) &&
            holds(value))
            return true;
    return false;
}

bool in_literals(const ArrayNode& values, const LiteralValue& v) {
    if (values.set) {
        const auto* s = std::get_if<std::string>(&v);
        return s && values.set->values.contains(*s);
    }
    for (const auto& elem : values.elements) {
        auto cmp = compare_literals(v, elem.value);
        if (cmp && *cmp == 0) return true;
    }
    return false;
}

bool eval_map_node(const QueryNode& node, const ValueMap& fields) {
    return std::visit(
        [&fields](auto&& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, CompareNode>) {
                return map_holds(n.field, fields, [&](const LiteralValue& v) {
                    return apply_compare(n.op,
                                         compare_literals(v, n.value.value));
                });
            } else if constexpr (std::is_same_v<T, InNode>) {
                return map_holds(n.field, fields, [&](const LiteralValue& v) {
                    return in_literals(n.values, v);
                });
            } else if constexpr (std::is_same_v<T, NotInNode>) {
                return map_holds(n.field, fields, [&](const LiteralValue& v) {
                    return !in_literals(n.values, v);
                });
            } else if constexpr (std::is_same_v<T, MatchNode>) {
                if (!n.compiled) return false;
                return map_holds(n.field, fields, [&](const LiteralValue& v) {
                    const auto* s = std::get_if<std::string>(&v);
                    if (!s) return false;
                    bool m =
                        std::regex_search(s->begin(), s->end(), n.compiled->re);
                    return n.negated ? !m : m;
                });
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

}  // namespace dftracer::utils::query
