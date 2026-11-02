// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_AST_H
#define DFTRACER_UTILS_DUQL_AST_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/variant.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <variant>

namespace dftracer::utils::duql {

/// Comparison operators for query expressions.
enum class CompareOp { Eq, Ne, Gt, Lt, Ge, Le };

/// Pattern-match operators. LIKE/ILIKE use SQL wildcards (% = any run, _ = one
/// UTF-8 character) and match the whole string; REGEX/IREGEX use the duql
/// regex dialect and search anywhere in the value; ICONTAINS is
/// case-insensitive substring containment (the "'sub' in field" form). The I*
/// variants are case-insensitive.
enum class MatchOp { Like, Ilike, Regex, Iregex, Icontains };

/// A field reference (e.g., "cat", "args.level"), or `any(path)`: the leaf
/// holds when any scalar element of the array at `path` satisfies it.
struct FieldNode {
    dstd::String path;  ///< Dotted path into JSON.
    bool any = false;
};

/// The field that `text` names: `any(<path>)` or a plain path.
inline FieldNode field_node(dstd::StringView text) {
    constexpr dstd::StringView ANY_OPEN = "any(";
    if (text.size() > ANY_OPEN.size() + 1 && text.starts_with(ANY_OPEN) &&
        text.ends_with(')'))
        return FieldNode{
            dstd::String(text.substr(ANY_OPEN.size(),
                                     text.size() - ANY_OPEN.size() - 1)),
            true};
    return FieldNode{dstd::String(text)};
}

/// The field as a query writes it.
inline dstd::String field_text(const FieldNode& field) {
    return field.any ? "any(" + field.path + ")" : field.path;
}

/// A typed literal value in a query expression.
using LiteralValue =
    dstd::Variant<dstd::String, int64_t, uint64_t, double, bool>;

/// A value bound to a parameter: a scalar, or a list that `x in $p` reads.
using ParamValue = dstd::Variant<LiteralValue, dstd::Vector<LiteralValue>>;

/// A literal value node.
struct LiteralNode {
    LiteralValue value;
};

/// The values of a long all-string list, for constant-time membership.
struct InSet;

/// An array of literal values (used by in/not-in).
struct ArrayNode {
    dstd::Vector<LiteralNode> elements;
    /// Built by make_in_set; null for a short or mixed-type list.
    dstd::SharedPtr<const InSet> set;
};

struct QueryNode;
using QueryNodePtr = dstd::UniquePtr<QueryNode>;

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

/// Compiled regex backing a @ref MatchNode.
struct CompiledPattern;

/// field like/ilike/~/~* pattern (e.g., name like "%Send%").
struct MatchNode {
    FieldNode field;
    MatchOp op;
    dstd::String pattern;  ///< Original pattern text (for round-trip).
    bool negated = false;  ///< True for "not like"/"not ilike"/!~/!~* forms.
    dstd::Optional<char> escape;  ///< A like/ilike `escape` character.
    dstd::SharedPtr<const CompiledPattern> compiled;  ///< Precompiled matcher.
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
    dstd::SharedPtr<const Term> term;
    dstd::String text;  ///< Its duql text, which parses back to it.
};

using QueryNodeVariant =
    dstd::Variant<CompareNode, InNode, NotInNode, MatchNode, AndNode, OrNode,
                  NotNode, ExprLeaf>;

/// Sum type for all query AST nodes.
struct QueryNode {
    QueryNodeVariant data;

    template <typename T>
    explicit QueryNode(T&& val) : data(std::forward<T>(val)) {}
};

/// Create a heap-allocated @ref QueryNode.
template <typename T>
QueryNodePtr make_node(T&& val) {
    return dstd::make_unique<QueryNode>(std::forward<T>(val));
}

/// Human-readable string for a CompareOp (e.g., "==", "!=").
const char* compare_op_str(CompareOp op);

/// Serialize an AST back to query DSL string.
dstd::String to_string(const QueryNode& node);

}  // namespace dftracer::utils::duql

// Inline compare_op_str/to_string, included last so the structs above are
// visible; keeps the DSL renderer usable by plugins that link nothing.
#include <dftracer/utils/duql/ast_string.h>

#endif  // DFTRACER_UTILS_DUQL_AST_H
