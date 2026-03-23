#include <dftracer/utils/utilities/common/query/ast.h>

#include <sstream>
#include <string>

namespace dftracer::utils::utilities::common::query {

const char* compare_op_str(CompareOp op) {
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

namespace {

void literal_to_string(std::ostringstream& os, const LiteralNode& lit) {
    std::visit(
        [&os](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                os << '"' << v << '"';
            } else if constexpr (std::is_same_v<T, bool>) {
                os << (v ? "true" : "false");
            } else if constexpr (std::is_same_v<T, int64_t>) {
                os << v;
            } else if constexpr (std::is_same_v<T, uint64_t>) {
                os << v;
            } else if constexpr (std::is_same_v<T, double>) {
                os << v;
            }
        },
        lit.value);
}

void array_to_string(std::ostringstream& os, const ArrayNode& arr) {
    os << '[';
    for (std::size_t i = 0; i < arr.elements.size(); ++i) {
        if (i > 0) os << ", ";
        literal_to_string(os, arr.elements[i]);
    }
    os << ']';
}

void node_to_string(std::ostringstream& os, const QueryNode& node) {
    std::visit(
        [&os](auto&& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, CompareNode>) {
                os << n.field.path << ' ' << compare_op_str(n.op) << ' ';
                literal_to_string(os, n.value);
            } else if constexpr (std::is_same_v<T, InNode>) {
                os << n.field.path << " in ";
                array_to_string(os, n.values);
            } else if constexpr (std::is_same_v<T, NotInNode>) {
                os << n.field.path << " not in ";
                array_to_string(os, n.values);
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

}  // namespace

std::string to_string(const QueryNode& node) {
    std::ostringstream os;
    node_to_string(os, node);
    return os.str();
}

}  // namespace dftracer::utils::utilities::common::query
