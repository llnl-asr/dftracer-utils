#ifndef DFTRACER_UTILS_UTILITIES_COMMON_QUERY_AST_H
#define DFTRACER_UTILS_UTILITIES_COMMON_QUERY_AST_H

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace dftracer::utils::utilities::common::query {

enum class CompareOp { EQ, NE, GT, LT, GE, LE };

struct FieldNode {
    std::string path;
};

using LiteralValue = std::variant<std::string, int64_t, uint64_t, double, bool>;

struct LiteralNode {
    LiteralValue value;
};

struct ArrayNode {
    std::vector<LiteralNode> elements;
};

struct QueryNode;
using QueryNodePtr = std::unique_ptr<QueryNode>;

struct CompareNode {
    FieldNode field;
    CompareOp op;
    LiteralNode value;
};

struct InNode {
    FieldNode field;
    ArrayNode values;
};

struct NotInNode {
    FieldNode field;
    ArrayNode values;
};

struct AndNode {
    QueryNodePtr left;
    QueryNodePtr right;
};

struct OrNode {
    QueryNodePtr left;
    QueryNodePtr right;
};

struct NotNode {
    QueryNodePtr operand;
};

using QueryNodeVariant =
    std::variant<CompareNode, InNode, NotInNode, AndNode, OrNode, NotNode>;

struct QueryNode {
    QueryNodeVariant data;

    template <typename T>
    explicit QueryNode(T&& val) : data(std::forward<T>(val)) {}
};

template <typename T>
QueryNodePtr make_node(T&& val) {
    return std::make_unique<QueryNode>(std::forward<T>(val));
}

const char* compare_op_str(CompareOp op);
std::string to_string(const QueryNode& node);

}  // namespace dftracer::utils::utilities::common::query

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_QUERY_AST_H
