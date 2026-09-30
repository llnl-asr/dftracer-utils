#ifndef DFTRACER_UTILS_DUQL_AST_H
#define DFTRACER_UTILS_DUQL_AST_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dftracer::utils::duql {

/// Comparison operators for query expressions.
enum class CompareOp { EQ, NE, GT, LT, GE, LE };

/// Pattern-match operators. LIKE/ILIKE use SQL wildcards (% = any run, _ = one
/// UTF-8 character) and match the whole string; REGEX/IREGEX use the duql
/// regex dialect and search anywhere in the value; ICONTAINS is
/// case-insensitive substring containment (the "'sub' in field" form). The I*
/// variants are case-insensitive.
enum class MatchOp { LIKE, ILIKE, REGEX, IREGEX, ICONTAINS };

/// A field reference (e.g., "cat", "args.level"), or `any(path)`: the leaf
/// holds when any scalar element of the array at `path` satisfies it.
struct FieldNode {
    std::string path;  ///< Dotted path into JSON.
    bool any = false;
};

/// The field that `text` names: `any(<path>)` or a plain path.
inline FieldNode field_node(std::string_view text) {
    constexpr std::string_view ANY_OPEN = "any(";
    if (text.size() > ANY_OPEN.size() + 1 && text.starts_with(ANY_OPEN) &&
        text.ends_with(')'))
        return FieldNode{
            std::string(text.substr(ANY_OPEN.size(),
                                    text.size() - ANY_OPEN.size() - 1)),
            true};
    return FieldNode{std::string(text)};
}

/// The field as a query writes it.
inline std::string field_text(const FieldNode& field) {
    return field.any ? "any(" + field.path + ")" : field.path;
}

/// A typed literal value in a query expression.
using LiteralValue = std::variant<std::string, int64_t, uint64_t, double, bool>;

/// A value bound to a parameter: a scalar, or a list that `x in $p` reads.
using ParamValue = std::variant<LiteralValue, std::vector<LiteralValue>>;

/// A literal value node.
struct LiteralNode {
    LiteralValue value;
};

/// The values of a long all-string list, for constant-time membership.
struct InSet;

/// An array of literal values (used by in/not-in).
struct ArrayNode {
    std::vector<LiteralNode> elements;
    /// Built by make_in_set; null for a short or mixed-type list.
    std::shared_ptr<const InSet> set;
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

/// Compiled regex backing a MatchNode.
struct CompiledPattern;

/// field like/ilike/~/~* pattern (e.g., name like "%Send%").
struct MatchNode {
    FieldNode field;
    MatchOp op;
    std::string pattern;   ///< Original pattern text (for round-trip).
    bool negated = false;  ///< True for "not like"/"not ilike"/!~/!~* forms.
    std::optional<char> escape;  ///< A like/ilike `escape` character.
    std::shared_ptr<const CompiledPattern> compiled;  ///< Precompiled matcher.
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

/// The evaluated form of an expression; defined inside the library.
struct Term;

/// A condition that is not one field against literals: arithmetic, a
/// function, field against field, `between`, `is`, `??`. It never prunes.
struct ExprLeaf {
    std::shared_ptr<const Term> term;
    std::string text;  ///< Its duql text, which parses back to it.
};

using QueryNodeVariant = std::variant<CompareNode, InNode, NotInNode, MatchNode,
                                      AndNode, OrNode, NotNode, ExprLeaf>;

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

}  // namespace dftracer::utils::duql

// Inline compare_op_str/to_string, included last so the structs above are
// visible; keeps the DSL renderer usable by plugins that link nothing.
#include <dftracer/utils/duql/ast_string.h>

#endif  // DFTRACER_UTILS_DUQL_AST_H
