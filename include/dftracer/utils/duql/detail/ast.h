// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_DETAIL_AST_H
#define DFTRACER_UTILS_DUQL_DETAIL_AST_H

#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/variant.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/types.h>

#include <cstdint>
#include <variant>

namespace dftracer::utils::duql {

/// The values of a long all-string list, for constant-time membership.
struct InSet;

/// Compiled regex backing a @ref MatchNode.
struct CompiledPattern;

/// The evaluated form of an expression; defined inside the library.
struct Term;

namespace detail {

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
    return field.any ? dstd::fmt<"any({})">(field.path) : field.path;
}

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

}  // namespace detail

}  // namespace dftracer::utils::duql

// Inline compare_op_str/to_string, included last so the structs above are
// visible; keeps the DSL renderer usable by plugins that link nothing.
#include <dftracer/utils/duql/detail/ast_string.h>

#endif  // DFTRACER_UTILS_DUQL_DETAIL_AST_H
