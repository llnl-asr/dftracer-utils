#ifndef DFTRACER_UTILS_QUERY_AST_STRING_H
#define DFTRACER_UTILS_QUERY_AST_STRING_H

#include <dftracer/utils/query/ast.h>

#include <cstddef>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>

// Header-only renderer from a query AST back to the DSL string. Inline and free
// of any non-inline dependency, so a plugin renders an Expr with include/ only
// and links nothing.
namespace dftracer::utils::query {

namespace detail {

// Operator keyword for a like/regex MatchNode ("in" is handled separately since
// its serialization is literal-first).
inline const char* match_op_str(MatchOp op, bool negated) {
    switch (op) {
        case MatchOp::LIKE:
            return negated ? "not like" : "like";
        case MatchOp::ILIKE:
            return negated ? "not ilike" : "ilike";
        case MatchOp::REGEX:
            return negated ? "!~" : "~";
        case MatchOp::IREGEX:
            return negated ? "!~*" : "~*";
        case MatchOp::ICONTAINS:
            break;
    }
    return "??";
}

inline void literal_to_string(std::ostringstream& os, const LiteralNode& lit) {
    std::visit(
        [&os](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                os << '"' << v << '"';
            } else if constexpr (std::is_same_v<T, bool>) {
                os << (v ? "true" : "false");
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                os << v;
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                os << v;
            } else if constexpr (std::is_same_v<T, double>) {
                os << v;
            }
        },
        lit.value);
}

inline void array_to_string(std::ostringstream& os, const ArrayNode& arr) {
    os << '[';
    for (std::size_t i = 0; i < arr.elements.size(); ++i) {
        if (i > 0) os << ", ";
        literal_to_string(os, arr.elements[i]);
    }
    os << ']';
}

inline void node_to_string(std::ostringstream& os, const QueryNode& node) {
    std::visit(
        [&os](auto&& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, CompareNode>) {
                os << field_text(n.field) << ' ' << compare_op_str(n.op) << ' ';
                literal_to_string(os, n.value);
            } else if constexpr (std::is_same_v<T, InNode>) {
                os << field_text(n.field) << " in ";
                array_to_string(os, n.values);
            } else if constexpr (std::is_same_v<T, NotInNode>) {
                os << field_text(n.field) << " not in ";
                array_to_string(os, n.values);
            } else if constexpr (std::is_same_v<T, MatchNode>) {
                if (n.op == MatchOp::ICONTAINS) {
                    os << '"' << n.pattern
                       << (n.negated ? "\" not in " : "\" in ")
                       << field_text(n.field);
                } else {
                    os << field_text(n.field) << ' '
                       << match_op_str(n.op, n.negated) << " \"" << n.pattern
                       << '"';
                }
            } else if constexpr (std::is_same_v<T, AndNode>) {
                os << '(';
                node_to_string(os, *n.left);
                os << " and ";
                node_to_string(os, *n.right);
                os << ')';
            } else if constexpr (std::is_same_v<T, OrNode>) {
                os << '(';
                node_to_string(os, *n.left);
                os << " or ";
                node_to_string(os, *n.right);
                os << ')';
            } else if constexpr (std::is_same_v<T, NotNode>) {
                os << "not (";
                node_to_string(os, *n.operand);
                os << ')';
            }
        },
        node.data);
}

}  // namespace detail

/// Human-readable string for a CompareOp (e.g., "==", "!=").
inline const char* compare_op_str(CompareOp op) {
    switch (op) {
        case CompareOp::EQ:
            return "==";
        case CompareOp::NE:
            return "!=";
        case CompareOp::GT:
            return ">";
        case CompareOp::LT:
            return "<";
        case CompareOp::GE:
            return ">=";
        case CompareOp::LE:
            return "<=";
    }
    return "??";
}

/// Serialize an AST back to query DSL string.
inline std::string to_string(const QueryNode& node) {
    std::ostringstream os;
    detail::node_to_string(os, node);
    return os.str();
}

}  // namespace dftracer::utils::query

#endif  // DFTRACER_UTILS_QUERY_AST_STRING_H
