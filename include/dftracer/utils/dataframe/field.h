#ifndef DFTRACER_UTILS_DATAFRAME_FIELD_H
#define DFTRACER_UTILS_DATAFRAME_FIELD_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/query/builder.h>
#include <dftracer/utils/query/query.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// The unified, name-based field entry point `F`: one expression type for both
// index-pushable filter predicates and in-memory columnar value expressions,
// mirroring the Python dftracer.utils.columnar.F.
//
// This is a strict superset of the predicate-only query::F (query/builder.h):
// same predicate meaning, plus value ops. It lives in the dataframe layer
// (which depends on query) so it can lower predicates through query::Expr and
// values through dataframe::Expr. query::F stays the F that plugins use
// header-only, with no dataframe link; this F is for callers that already link
// the engine.
namespace dftracer::utils::dataframe::field {

namespace detail {

/// A node in the unified field-expression tree; each subtree lowers to a
/// dataframe::Expr (value ops) or a query::Expr (predicate ops), and raises
/// when lowered to the side it does not belong to.
enum class FKind : std::uint8_t {
    Col,
    LitI,
    LitF,
    Bin,   // arithmetic: op == dataframe::BinaryOp
    Prim,  // unary numeric primitive: op == dftu_prim_op
    Cmp,   // field/value compared to a scalar: op == query::CompareOp
    And,
    Or,
    Not,
    Match,  // string-match predicate: op == query::MatchOp
    In      // membership predicate
};

/// Right-hand-side scalar type of a Cmp node. A numeric rhs has an in-memory
/// mask form; a string/bool rhs is filter-only (predicate pushdown only).
enum class RhsKind : std::uint8_t { Int, Float, Str, Bool };

struct FNode {
    FKind kind;
    std::string name;            // Col / Match / In field name
    std::int64_t ival = 0;       // LitI, or Int-rhs Cmp value
    double dval = 0.0;           // LitF, or Float-rhs Cmp value
    int op = 0;                  // Bin / Prim / Cmp / Match op code
    RhsKind rhs = RhsKind::Int;  // Cmp rhs type
    bool bval = false;           // Bool-rhs Cmp value
    std::string str;             // Str-rhs Cmp / Match pattern
    bool negated = false;        // In: not-in
    bool in_is_string = false;   // In: string vs int element type
    std::vector<std::int64_t> in_ints;
    std::vector<std::string> in_strs;
    std::shared_ptr<const FNode> a;
    std::shared_ptr<const FNode> b;
};

using FNodePtr = std::shared_ptr<const FNode>;

template <typename... Fields>
inline FNodePtr make_node(FKind kind, Fields&&... init) {
    auto n = std::make_shared<FNode>();
    n->kind = kind;
    (init(*n), ...);
    return n;
}

}  // namespace detail

/// The reduction of an aggregate expression (`F("dur").sum()`, `F.any.mean()`).
/// Maps 1:1 to trace::views::AggOp; the trace layer lowers each to an AggSpec.
enum class AggFn : std::uint8_t {
    Count,
    Sum,
    Min,
    Max,
    Mean,
    Var,
    Std,
    Skew,
    Kurt,
    ArgMax
};

/// An aggregate over a named field (or the numeric-args wildcard, `F.any`),
/// produced by the `F(...).sum()/mean()/...` builders and consumed by
/// View::group_by(...).agg(...). Value-typed: it carries the reduction, the
/// field to reduce (empty for Count and for the wildcard), the `by` field for
/// ArgMax, and the wildcard flag.
class FieldAggExpr {
   public:
    FieldAggExpr(AggFn fn, std::string field, bool wildcard = false,
                 std::string by = "")
        : fn_(fn),
          field_(std::move(field)),
          by_(std::move(by)),
          wildcard_(wildcard) {}

    AggFn fn() const noexcept { return fn_; }
    const std::string& field() const noexcept { return field_; }
    const std::string& by() const noexcept { return by_; }
    bool wildcard() const noexcept { return wildcard_; }

   private:
    AggFn fn_;
    std::string field_;
    std::string by_;
    bool wildcard_;
};

/// The unified field expression. Build values with `+ - * /` and the numeric
/// prims (ilog2/bit_width/popcount/clz/ctz/mix64), or predicates with the
/// comparisons (`> >= < <= == !=`), string matches (like/ilike/regex/contains),
/// membership (in/not_in), and `&& || !`. Evaluate a value or a numeric
/// comparison mask in memory with apply(df); serialize a pure predicate for
/// index pushdown with to_query(). Value-semantics handle over a shared,
/// immutable node.
class FieldExpr {
   public:
    FieldExpr() = default;
    explicit FieldExpr(detail::FNodePtr node) : node_(std::move(node)) {}

    bool valid() const noexcept { return node_ != nullptr; }
    const detail::FNodePtr& node() const noexcept { return node_; }

    // Comparisons -> a predicate. A numeric rhs also has an in-memory mask form
    // (apply); a string/bool rhs is filter-only.
    template <typename T>
    FieldExpr operator==(T&& v) const {
        return cmp(query::CompareOp::EQ, std::forward<T>(v));
    }
    template <typename T>
    FieldExpr operator!=(T&& v) const {
        return cmp(query::CompareOp::NE, std::forward<T>(v));
    }
    template <typename T>
    FieldExpr operator>(T&& v) const {
        return cmp(query::CompareOp::GT, std::forward<T>(v));
    }
    template <typename T>
    FieldExpr operator<(T&& v) const {
        return cmp(query::CompareOp::LT, std::forward<T>(v));
    }
    template <typename T>
    FieldExpr operator>=(T&& v) const {
        return cmp(query::CompareOp::GE, std::forward<T>(v));
    }
    template <typename T>
    FieldExpr operator<=(T&& v) const {
        return cmp(query::CompareOp::LE, std::forward<T>(v));
    }

    /// Equality/inequality predicate in method form (parity with the Python
    /// `.eq()` / `.ne()`).
    template <typename T>
    FieldExpr eq(T&& v) const {
        return cmp(query::CompareOp::EQ, std::forward<T>(v));
    }
    template <typename T>
    FieldExpr ne(T&& v) const {
        return cmp(query::CompareOp::NE, std::forward<T>(v));
    }

    /// Membership predicates (filter-only). The field is (not) one of `values`.
    FieldExpr in(const std::vector<std::int64_t>& values) const {
        return in_impl(values, /*negated=*/false);
    }
    FieldExpr in(const std::vector<std::string>& values) const {
        return in_impl(values, /*negated=*/false);
    }
    FieldExpr not_in(const std::vector<std::int64_t>& values) const {
        return in_impl(values, /*negated=*/true);
    }
    FieldExpr not_in(const std::vector<std::string>& values) const {
        return in_impl(values, /*negated=*/true);
    }

    /// String-match predicates (filter-only): SQL LIKE / ILIKE, ECMAScript
    /// regex / case-insensitive regex, and case-insensitive substring.
    FieldExpr like(std::string_view pattern) const {
        return match(query::MatchOp::LIKE, pattern);
    }
    FieldExpr ilike(std::string_view pattern) const {
        return match(query::MatchOp::ILIKE, pattern);
    }
    FieldExpr regex(std::string_view pattern) const {
        return match(query::MatchOp::REGEX, pattern);
    }
    FieldExpr iregex(std::string_view pattern) const {
        return match(query::MatchOp::IREGEX, pattern);
    }
    FieldExpr contains(std::string_view substring) const {
        return match(query::MatchOp::ICONTAINS, substring);
    }

    /// Unary numeric primitives -> a value expression (evaluate with apply).
    FieldExpr ilog2() const { return prim(DFTU_PRIM_ILOG2); }
    FieldExpr bit_width() const { return prim(DFTU_PRIM_BIT_WIDTH); }
    FieldExpr popcount() const { return prim(DFTU_PRIM_POPCOUNT); }
    FieldExpr clz() const { return prim(DFTU_PRIM_CLZ); }
    FieldExpr ctz() const { return prim(DFTU_PRIM_CTZ); }
    FieldExpr mix64() const { return prim(DFTU_PRIM_MIX64); }

    /// Aggregate reductions -> a FieldAggExpr for View::group_by(...).agg().
    /// Each reduces the bare field this expression names (a value expression is
    /// rejected: name a plain field, e.g. F("dur").sum()). This is additive to
    /// the string / AggSpec agg forms, which stay fully supported.
    FieldAggExpr sum() const { return agg_of(AggFn::Sum); }
    FieldAggExpr min() const { return agg_of(AggFn::Min); }
    FieldAggExpr max() const { return agg_of(AggFn::Max); }
    FieldAggExpr mean() const { return agg_of(AggFn::Mean); }
    FieldAggExpr var() const { return agg_of(AggFn::Var); }
    FieldAggExpr std() const { return agg_of(AggFn::Std); }
    FieldAggExpr skew() const { return agg_of(AggFn::Skew); }
    FieldAggExpr kurt() const { return agg_of(AggFn::Kurt); }
    /// Group row count (the field is ignored, matching the engine's Count).
    FieldAggExpr count() const { return FieldAggExpr(AggFn::Count, ""); }
    /// The value of this field at the row where `by` is maximal (ArgMax). No
    /// argmin: the engine's AggOp has only ArgMax.
    FieldAggExpr argmax_by(std::string_view by) const {
        return FieldAggExpr(AggFn::ArgMax, field_name(), false,
                            std::string(by));
    }

    /// Serialize this predicate to an index-pushable query::Query.
    ///
    /// Throws DFTUtilsException(INVALID_ARGUMENT) if the expression mixes value
    /// ops (arithmetic or a numeric primitive) into the predicate - i.e. it is
    /// not pushable; evaluate those in memory with apply() instead.
    query::Query to_query() const {
        query::Expr e = to_query_expr(require());
        auto q = e.build();
        if (!q.has_value()) {
            throw DFTUtilsException::cat(
                ErrorCode::INVALID_ARGUMENT,
                "failed to build query from field expression");
        }
        return std::move(q.value());
    }

    /// Evaluate this expression on `df` and return a Series.
    ///
    /// A value expression yields a value column; a numeric comparison yields a
    /// bit-packed Bool mask (for DataFrame::filter). The field NAMES are
    /// resolved to `df`'s column indices and bridged to dataframe::Expr, then
    /// compiled and evaluated in one fused pass (dataframe::eval).
    ///
    /// Throws DFTUtilsException(INVALID_ARGUMENT) for a filter-only predicate
    /// (string match, membership, or a string/bool comparison), which has no
    /// in-memory form (push those down with View::filter / to_query), or when a
    /// referenced column is absent from `df`.
    Series apply(const DataFrame& df) const {
        const detail::FNode& root = require();
        std::vector<std::string> names;
        collect_value_columns(root, names);

        std::unordered_map<std::string, std::int32_t> index;
        std::vector<Series> held;
        held.reserve(names.size());
        for (std::size_t i = 0; i < names.size(); ++i) {
            std::int64_t idx = df.column_index(names[i]);
            if (idx < 0) {
                throw DFTUtilsException::cat(
                    ErrorCode::INVALID_ARGUMENT,
                    "field expression references column not present in frame: ",
                    names[i]);
            }
            index.emplace(names[i], static_cast<std::int32_t>(i));
            held.push_back(df.column(names[i]));
        }

        std::vector<const Series*> inputs;
        inputs.reserve(held.size());
        for (const Series& s : held) inputs.push_back(&s);

        Expr value = to_value_expr(root, index);
        return eval(value, inputs);
    }

   private:
    detail::FNodePtr node_;

    const detail::FNode& require() const {
        if (!node_) {
            throw DFTUtilsException::cat(ErrorCode::INVALID_ARGUMENT,
                                         "empty field expression");
        }
        return *node_;
    }

    template <typename T>
    FieldExpr cmp(query::CompareOp op, T&& v) const {
        using D = std::decay_t<T>;
        auto init = [&](detail::FNode& n) {
            n.op = static_cast<int>(op);
            n.a = node_;
            if constexpr (std::is_same_v<D, bool>) {
                n.rhs = detail::RhsKind::Bool;
                n.bval = v;
            } else if constexpr (std::is_floating_point_v<D>) {
                n.rhs = detail::RhsKind::Float;
                n.dval = static_cast<double>(v);
            } else if constexpr (std::is_integral_v<D>) {
                n.rhs = detail::RhsKind::Int;
                n.ival = static_cast<std::int64_t>(v);
            } else {
                n.rhs = detail::RhsKind::Str;
                n.str = std::string(std::forward<T>(v));
            }
        };
        return FieldExpr(detail::make_node(detail::FKind::Cmp, init));
    }

    FieldExpr prim(dftu_prim_op code) const {
        return FieldExpr(
            detail::make_node(detail::FKind::Prim, [&](detail::FNode& n) {
                n.op = static_cast<int>(code);
                n.a = node_;
            }));
    }

    FieldExpr match(query::MatchOp op, std::string_view pattern) const {
        return FieldExpr(
            detail::make_node(detail::FKind::Match, [&](detail::FNode& n) {
                n.op = static_cast<int>(op);
                n.name = field_name();
                n.str = std::string(pattern);
            }));
    }

    template <typename V>
    FieldExpr in_impl(const std::vector<V>& values, bool negated) const {
        return FieldExpr(
            detail::make_node(detail::FKind::In, [&](detail::FNode& n) {
                n.name = field_name();
                n.negated = negated;
                if constexpr (std::is_same_v<V, std::string>) {
                    n.in_is_string = true;
                    n.in_strs = values;
                } else {
                    n.in_is_string = false;
                    n.in_ints.reserve(values.size());
                    for (V v : values)
                        n.in_ints.push_back(static_cast<std::int64_t>(v));
                }
            }));
    }

    /// Bare-column name for the predicate-only ops (match/membership), which
    /// require a field leaf on the left.
    std::string field_name() const {
        const detail::FNode& n = require();
        if (n.kind != detail::FKind::Col) {
            throw DFTUtilsException::cat(
                ErrorCode::INVALID_ARGUMENT,
                "string-match / membership needs a bare field on the left");
        }
        return n.name;
    }

    FieldAggExpr agg_of(AggFn fn) const {
        const detail::FNode& n = require();
        if (n.kind != detail::FKind::Col) {
            throw DFTUtilsException::cat(
                ErrorCode::INVALID_ARGUMENT,
                "aggregate needs a bare field, e.g. F(\"dur\").sum()");
        }
        return FieldAggExpr(fn, n.name);
    }

    static query::Expr to_query_expr(const detail::FNode& n) {
        using detail::FKind;
        switch (n.kind) {
            case FKind::Cmp:
                return cmp_to_query(n);
            case FKind::And:
                return query::all_of(to_query_expr(*n.a), to_query_expr(*n.b));
            case FKind::Or:
                return query::any_of(to_query_expr(*n.a), to_query_expr(*n.b));
            case FKind::Not:
                return query::negate(to_query_expr(*n.a));
            case FKind::Match:
                return query::field_match(
                    n.name, static_cast<query::MatchOp>(n.op), n.str);
            case FKind::In:
                if (n.in_is_string) {
                    return n.negated ? query::field_not_in(n.name, n.in_strs)
                                     : query::field_in(n.name, n.in_strs);
                }
                return n.negated ? query::field_not_in(n.name, n.in_ints)
                                 : query::field_in(n.name, n.in_ints);
            default:
                throw DFTUtilsException::cat(
                    ErrorCode::INVALID_ARGUMENT,
                    "not an index-pushable predicate; compute it with apply() "
                    "or filter the materialized frame");
        }
    }

    static query::Expr cmp_to_query(const detail::FNode& n) {
        if (!n.a || n.a->kind != detail::FKind::Col) {
            throw DFTUtilsException::cat(
                ErrorCode::INVALID_ARGUMENT,
                "not an index-pushable predicate; compute it with apply() or "
                "filter the materialized frame");
        }
        auto op = static_cast<query::CompareOp>(n.op);
        switch (n.rhs) {
            case detail::RhsKind::Int:
                return query::field_cmp(n.a->name, op,
                                        query::detail::literal(n.ival));
            case detail::RhsKind::Float:
                return query::field_cmp(n.a->name, op,
                                        query::detail::literal(n.dval));
            case detail::RhsKind::Bool:
                return query::field_cmp(n.a->name, op,
                                        query::detail::literal(n.bval));
            case detail::RhsKind::Str:
                return query::field_cmp(n.a->name, op,
                                        query::detail::literal(n.str));
        }
        throw DFTUtilsException::cat(ErrorCode::INVALID_ARGUMENT,
                                     "unhandled comparison rhs");
    }

    static void collect_value_columns(const detail::FNode& n,
                                      std::vector<std::string>& out) {
        using detail::FKind;
        switch (n.kind) {
            case FKind::Col:
                for (const std::string& e : out)
                    if (e == n.name) return;
                out.push_back(n.name);
                return;
            case FKind::LitI:
            case FKind::LitF:
                return;
            case FKind::Bin:
            case FKind::And:
            case FKind::Or:
                collect_value_columns(*n.a, out);
                collect_value_columns(*n.b, out);
                return;
            case FKind::Prim:
            case FKind::Not:
                collect_value_columns(*n.a, out);
                return;
            case FKind::Cmp:
                if (n.rhs != detail::RhsKind::Int &&
                    n.rhs != detail::RhsKind::Float) {
                    throw DFTUtilsException::cat(
                        ErrorCode::INVALID_ARGUMENT,
                        "string/bool comparison is filter-only; push it down "
                        "with View::filter / to_query()");
                }
                collect_value_columns(*n.a, out);
                return;
            case FKind::Match:
            case FKind::In:
                throw DFTUtilsException::cat(
                    ErrorCode::INVALID_ARGUMENT,
                    "string-match / membership is filter-only; push it down "
                    "with View::filter / to_query()");
        }
    }

    static Expr to_value_expr(
        const detail::FNode& n,
        const std::unordered_map<std::string, std::int32_t>& index) {
        using detail::FKind;
        // Use the engine's expr_* builders explicitly: `lit` is shadowed by the
        // field-layer literal factory below, so unqualified `lit` here would be
        // the wrong overload.
        switch (n.kind) {
            case FKind::Col:
                return expr_col(index.at(n.name));
            case FKind::LitI:
                return expr_lit(n.ival);
            case FKind::LitF:
                return expr_lit(n.dval);
            case FKind::Bin:
                return expr_binary(static_cast<BinaryOp>(n.op),
                                   to_value_expr(*n.a, index),
                                   to_value_expr(*n.b, index));
            case FKind::Prim:
                return expr_prim(static_cast<PrimOp>(n.op),
                                 to_value_expr(*n.a, index));
            case FKind::Cmp:
                return expr_cmp(
                    cmp_code(static_cast<query::CompareOp>(n.op)),
                    to_value_expr(*n.a, index),
                    n.rhs == detail::RhsKind::Float
                        ? ::dftracer::utils::dataframe::detail::expr_scalar_d(
                              n.dval)
                        : ::dftracer::utils::dataframe::detail::expr_scalar_i(
                              n.ival));
            case FKind::And:
                return expr_logical(LogicalOp::And, to_value_expr(*n.a, index),
                                    to_value_expr(*n.b, index));
            case FKind::Or:
                return expr_logical(LogicalOp::Or, to_value_expr(*n.a, index),
                                    to_value_expr(*n.b, index));
            case FKind::Not:
                return expr_not(to_value_expr(*n.a, index));
            default:
                throw DFTUtilsException::cat(
                    ErrorCode::INVALID_ARGUMENT,
                    "filter-only predicate has no in-memory form");
        }
    }

    static CmpOp cmp_code(query::CompareOp op) {
        switch (op) {
            case query::CompareOp::EQ:
                return CmpOp::Eq;
            case query::CompareOp::NE:
                return CmpOp::Ne;
            case query::CompareOp::GT:
                return CmpOp::Gt;
            case query::CompareOp::LT:
                return CmpOp::Lt;
            case query::CompareOp::GE:
                return CmpOp::Ge;
            case query::CompareOp::LE:
                return CmpOp::Le;
        }
        return CmpOp::Eq;
    }
};

namespace detail {

inline FNodePtr lit_node(std::int64_t v) {
    return make_node(FKind::LitI, [&](FNode& n) { n.ival = v; });
}
inline FNodePtr lit_node(double v) {
    return make_node(FKind::LitF, [&](FNode& n) { n.dval = v; });
}

}  // namespace detail

/// A scalar literal leaf (kept distinct from a column name).
inline FieldExpr lit(std::int64_t v) { return FieldExpr(detail::lit_node(v)); }
inline FieldExpr lit(double v) { return FieldExpr(detail::lit_node(v)); }

inline FieldExpr make_bin(detail::FKind kind, const FieldExpr& a,
                          const FieldExpr& b) {
    return FieldExpr(detail::make_node(kind, [&](detail::FNode& n) {
        n.a = a.node();
        n.b = b.node();
    }));
}

inline FieldExpr make_arith(BinaryOp op, const FieldExpr& a,
                            const FieldExpr& b) {
    return FieldExpr(
        detail::make_node(detail::FKind::Bin, [&](detail::FNode& n) {
            n.op = static_cast<int>(op);
            n.a = a.node();
            n.b = b.node();
        }));
}

// Arithmetic -> a value expression. A scalar operand is wrapped as a literal,
// so `F("a") + 2`, `2 + F("a")`, and `F("a") + F("b")` all build a Bin node.
inline FieldExpr operator+(const FieldExpr& a, const FieldExpr& b) {
    return make_arith(BinaryOp::Add, a, b);
}
inline FieldExpr operator-(const FieldExpr& a, const FieldExpr& b) {
    return make_arith(BinaryOp::Sub, a, b);
}
inline FieldExpr operator*(const FieldExpr& a, const FieldExpr& b) {
    return make_arith(BinaryOp::Mul, a, b);
}
inline FieldExpr operator/(const FieldExpr& a, const FieldExpr& b) {
    return make_arith(BinaryOp::Div, a, b);
}

inline FieldExpr operator+(const FieldExpr& a, std::int64_t b) {
    return a + lit(b);
}
inline FieldExpr operator-(const FieldExpr& a, std::int64_t b) {
    return a - lit(b);
}
inline FieldExpr operator*(const FieldExpr& a, std::int64_t b) {
    return a * lit(b);
}
inline FieldExpr operator/(const FieldExpr& a, std::int64_t b) {
    return a / lit(b);
}
inline FieldExpr operator+(std::int64_t a, const FieldExpr& b) {
    return lit(a) + b;
}
inline FieldExpr operator-(std::int64_t a, const FieldExpr& b) {
    return lit(a) - b;
}
inline FieldExpr operator*(std::int64_t a, const FieldExpr& b) {
    return lit(a) * b;
}
inline FieldExpr operator/(std::int64_t a, const FieldExpr& b) {
    return lit(a) / b;
}
inline FieldExpr operator+(const FieldExpr& a, double b) { return a + lit(b); }
inline FieldExpr operator-(const FieldExpr& a, double b) { return a - lit(b); }
inline FieldExpr operator*(const FieldExpr& a, double b) { return a * lit(b); }
inline FieldExpr operator/(const FieldExpr& a, double b) { return a / lit(b); }
inline FieldExpr operator+(double a, const FieldExpr& b) { return lit(a) + b; }
inline FieldExpr operator-(double a, const FieldExpr& b) { return lit(a) - b; }
inline FieldExpr operator*(double a, const FieldExpr& b) { return lit(a) * b; }
inline FieldExpr operator/(double a, const FieldExpr& b) { return lit(a) / b; }

// Combine predicates (matching the query-layer C++ surface: `&& || !`).
inline FieldExpr operator&&(const FieldExpr& a, const FieldExpr& b) {
    return make_bin(detail::FKind::And, a, b);
}
inline FieldExpr operator||(const FieldExpr& a, const FieldExpr& b) {
    return make_bin(detail::FKind::Or, a, b);
}
inline FieldExpr operator!(const FieldExpr& a) {
    return FieldExpr(detail::make_node(
        detail::FKind::Not, [&](detail::FNode& n) { n.a = a.node(); }));
}

/// A field / column reference by name (the same leaf `F(name)` builds).
inline FieldExpr field(std::string_view name) {
    return FieldExpr(detail::make_node(
        detail::FKind::Col,
        [&](detail::FNode& n) { n.name = std::string(name); }));
}

/// A resolved virtual field (`resolved.<name>`), which the index rewrites to a
/// concrete hash lookup. Mirrors query::resolved and the Python resolved().
inline FieldExpr resolved(std::string_view name) {
    return field("resolved." + std::string(name));
}

/// The numeric-args wildcard, reached as `F.any`. `mean()` aggregates every
/// numeric args.* field as a per-group mean (the fields are discovered at scan
/// time); `count()` is the group row count. The engine's wildcard path computes
/// only the mean, so the other reductions are rejected by View::agg - name a
/// field explicitly (e.g. F("args.level").sum()) for those.
struct WildcardField {
    FieldAggExpr mean() const { return FieldAggExpr(AggFn::Mean, "", true); }
    FieldAggExpr count() const { return FieldAggExpr(AggFn::Count, "", true); }
    FieldAggExpr sum() const { return FieldAggExpr(AggFn::Sum, "", true); }
    FieldAggExpr min() const { return FieldAggExpr(AggFn::Min, "", true); }
    FieldAggExpr max() const { return FieldAggExpr(AggFn::Max, "", true); }
    FieldAggExpr var() const { return FieldAggExpr(AggFn::Var, "", true); }
    FieldAggExpr std() const { return FieldAggExpr(AggFn::Std, "", true); }
    FieldAggExpr skew() const { return FieldAggExpr(AggFn::Skew, "", true); }
    FieldAggExpr kurt() const { return FieldAggExpr(AggFn::Kurt, "", true); }
};

/// Call-form field entry point: `F("dur") > 1000` builds a field leaf, exactly
/// like `field("dur") > 1000`. The single, canonical F for the dataframe layer
/// - a superset of the predicate-only query::F. `F.any` is the numeric-args
/// wildcard (see WildcardField).
struct FieldFactory {
    FieldExpr operator()(std::string_view name) const { return field(name); }
    WildcardField any{};
};
inline constexpr FieldFactory F{};

}  // namespace dftracer::utils::dataframe::field

#endif  // DFTRACER_UTILS_DATAFRAME_FIELD_H
