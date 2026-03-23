#ifndef DFTRACER_UTILS_UTILITIES_COMMON_QUERY_AST_H
#define DFTRACER_UTILS_UTILITIES_COMMON_QUERY_AST_H

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace dftracer::utils::utilities::common::query {

/// Comparison operators for query expressions.
enum class CompareOp { EQ, NE, GT, LT, GE, LE };

/// A field reference (e.g., "cat", "args.level").
struct FieldNode {
    std::string path;  ///< Dotted path into JSON.
};

/// A typed literal value in a query expression.
using LiteralValue = std::variant<std::string, int64_t, uint64_t, double, bool>;

/// A literal value node.
struct LiteralNode {
    LiteralValue value;
};

/// An array of literal values (used by in/not-in).
struct ArrayNode {
    std::vector<LiteralNode> elements;
};

struct QueryNode;
using QueryNodePtr = std::unique_ptr<QueryNode>;

/// field op value (e.g., cat == "POSIX").
struct CompareNode {
    FieldNode field;
    CompareOp op;
    LiteralNode value;
};

/// field in [values] (e.g., cat in ["POSIX", "STDIO"]).
struct InNode {
    FieldNode field;
    ArrayNode values;
};

/// field not in [values].
struct NotInNode {
    FieldNode field;
    ArrayNode values;
};

/// left and right.
struct AndNode {
    QueryNodePtr left;
    QueryNodePtr right;
};

/// left or right.
struct OrNode {
    QueryNodePtr left;
    QueryNodePtr right;
};

/// not operand.
struct NotNode {
    QueryNodePtr operand;
};

using QueryNodeVariant =
    std::variant<CompareNode, InNode, NotInNode, AndNode, OrNode, NotNode>;

/// Sum type for all query AST nodes.
struct QueryNode {
    QueryNodeVariant data;

    template <typename T>
    explicit QueryNode(T&& val) : data(std::forward<T>(val)) {}
};

/// Create a heap-allocated QueryNode.
template <typename T>
QueryNodePtr make_node(T&& val) {
    return std::make_unique<QueryNode>(std::forward<T>(val));
}

/// Human-readable string for a CompareOp (e.g., "==", "!=").
const char* compare_op_str(CompareOp op);

/// Serialize an AST back to query DSL string.
std::string to_string(const QueryNode& node);

}  // namespace dftracer::utils::utilities::common::query

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_QUERY_AST_H
