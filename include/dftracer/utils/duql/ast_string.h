#ifndef DFTRACER_UTILS_DUQL_AST_STRING_H
#define DFTRACER_UTILS_DUQL_AST_STRING_H

#include <dftracer/utils/duql/ast.h>

#include <cstddef>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>

// Header-only renderer from a query AST back to the DSL string. Inline and free
// of any non-inline dependency, so a plugin renders an Expr with include/ only
// and links nothing.
namespace dftracer::utils::duql {

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

// A string literal in the quote that lets it parse back: strings keep their
// bytes as written, so one holding an unescaped '"' goes in single quotes.
inline void string_to_string(std::ostringstream& os, const std::string& v) {
    char q = '"';
    std::size_t slashes = 0;
    for (char c : v) {
        if (c == '"' && slashes % 2 == 0) {
            q = '\'';
            break;
        }
        slashes = c == '\\' ? slashes + 1 : 0;
    }
    os << q << v << q;
}

inline void literal_to_string(std::ostringstream& os, const LiteralNode& lit) {
    std::visit(
        [&os](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                string_to_string(os, v);
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
                    string_to_string(os, n.pattern);
                    os << (n.negated ? " not in " : " in ")
                       << field_text(n.field);
                } else {
                    os << field_text(n.field) << ' '
                       << match_op_str(n.op, n.negated) << ' ';
                    string_to_string(os, n.pattern);
                    if (n.escape) {
                        os << " escape ";
                        string_to_string(os, std::string(1, *n.escape));
                    }
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
            } else if constexpr (std::is_same_v<T, ExprLeaf>) {
                os << n.text;
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

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_AST_STRING_H
