#ifndef DFTRACER_UTILS_DATAFRAME_EXPR_H
#define DFTRACER_UTILS_DATAFRAME_EXPR_H

#include <dftracer/utils/core/common/export.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/series.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

// A native columnar expression engine. Expressions are built as a DAG (col
// refs, literals, arithmetic, primitives, comparisons, logical ops, casts),
// then compiled - type inference + common-subexpression elimination + lowering
// to a flat slot program - and evaluated in one chunked pass (parallel via the
// injected backend, see parallel.h). The whole compiler lives here in C++ so
// every consumer (the Python DSL, plugins, the distributed engine) gets the
// same fusion and CSE; language frontends only build the DAG.
namespace dftracer::utils::dataframe {

struct ExprNode;

/// A handle to an expression DAG node (shared, so subexpressions can be reused
/// and are naturally de-duplicated; the compiler also hash-conses structurally
/// identical nodes). Build with the free functions below.
class Expr {
   public:
    Expr() = default;
    explicit Expr(std::shared_ptr<const ExprNode> node)
        : node_(std::move(node)) {}
    const std::shared_ptr<const ExprNode>& node() const noexcept {
        return node_;
    }
    bool valid() const noexcept { return node_ != nullptr; }

   private:
    std::shared_ptr<const ExprNode> node_;
};

/// Binary arithmetic op codes (match dftu_expr_binary).
enum class BinaryOp { Add = 0, Sub = 1, Mul = 2, Div = 3 };

Expr expr_col(std::int32_t index);

/// If `e` is a bare column reference, its column index; otherwise -1. Lets a
/// caller take the input column directly (e.g. a string group key) instead of
/// routing it through the numeric evaluator.
std::int32_t expr_col_index(const Expr& e);

/// True if `e` reads input column `index` anywhere in its tree. Lets the query
/// planner tell whether a predicate depends on a given column.
bool expr_references(const Expr& e, std::int32_t index);

/// The highest column index `e` reads anywhere in its tree, or -1 when it
/// reads none. Lets a cursor tell whether a predicate fits its input's
/// columns before forwarding it upstream.
std::int32_t expr_max_col(const Expr& e);

/// If `e` is `col <cmp> scalar`, fill *col/*op/*rhs and return true. Lets the
/// planner run a trivial predicate as a direct Series kernel, skipping the
/// expression compiler.
bool expr_as_col_cmp(const Expr& e, std::int32_t* col, CmpOp* op, Scalar* rhs);

/// If `e` is `col <binary> col`, fill *op/*a/*b and return true.
bool expr_as_col_binary(const Expr& e, BinaryOp* op, std::int32_t* a,
                        std::int32_t* b);

/// If `e` is `a <logical> b`, fill *op/*a/*b and return true. Lets a source
/// walk a compound predicate to decide how much of it it can push.
bool expr_as_logical(const Expr& e, LogicalOp* op, Expr* a, Expr* b);

/// If `e` is `not a`, fill *a and return true.
bool expr_as_not(const Expr& e, Expr* a);

/// Rewrite every column reference `col(i)` to `col(old_to_new[i])`, leaving an
/// out-of-range index unchanged. Lets the planner renumber a predicate after
/// columns are dropped or reordered (projection pushdown). Returns a new Expr;
/// unchanged subtrees are shared.
Expr expr_remap_cols(const Expr& e,
                     const std::vector<std::int32_t>& old_to_new);
Expr expr_lit(std::int64_t value);
Expr expr_lit(double value);
Expr expr_binary(BinaryOp op, const Expr& a, const Expr& b);
Expr expr_prim(PrimOp prim, const Expr& a);
/// Unary numeric op (UnaryOp: abs/round/floor/ceil/log/sqrt/exp/sign/negate/
/// trunc/is_nan/is_finite/is_infinite).
Expr expr_unary(UnaryOp op, const Expr& a);
/// Clamp each value to [lo, hi].
Expr expr_clip(const Expr& a, Scalar lo, Scalar hi);
/// Replace nulls (validity bitmap) with a fill value.
Expr expr_fillna(const Expr& a, Scalar fill);
/// A string predicate: `a <op> pattern` (Series::str_contains /
/// str_starts_with / str_ends_with / str_like / str_matches), Bool. The node
/// owns a copy of `pattern`. Throws at compile time if the operand is not a
/// String or Binary column.
Expr expr_str_pred(StrPredOp op, const Expr& a, std::string_view pattern);
/// A String -> String map (Series::to_lowercase / to_uppercase / str_strip /
/// str_lstrip / str_rstrip). Throws at compile time if the operand is not a
/// String column.
Expr expr_str_map(StrMapOp op, const Expr& a);
/// ASCII-lowercase a String column: expr_str_map(StrMapOp::Lower, a).
Expr expr_lower(const Expr& a);
/// Per-row length as Int64: bytes, or UTF-8 codepoints when `chars`.
Expr expr_str_len(const Expr& a, bool chars);
/// Byte index of the first `needle` per row, or -1, as Int64.
Expr expr_str_find(const Expr& a, std::string_view needle);
/// Replace the first (or with `all`, every) occurrence of `from` with `to`.
Expr expr_str_replace(const Expr& a, std::string_view from, std::string_view to,
                      bool all);
/// The byte substring [start, start + len) of each row (Series::str_slice).
Expr expr_str_slice(const Expr& a, std::int64_t start, std::int64_t len);
/// Membership: true where the row's value is one of `values`
/// (Series::is_in), Bool. The node shares `values`. Throws at compile time
/// if `values` is not in the operand's value domain (numeric vs string).
Expr expr_is_in(const Expr& a, Series values);

/// Per row, `a` where `cond` (a Bool expression) is true, else `b`
/// (`cond ? a : b`; SQL CASE, polars when / then / otherwise). `a` and `b`
/// are promoted to one type as arithmetic promotes them; either may be a
/// literal, which broadcasts. Throws at compile time if `cond` is not Bool.
Expr expr_select(const Expr& cond, const Expr& a, const Expr& b);

/// The Bool mask of the rows where `a` is null (`null` true) or present
/// (`null` false): the null_mask / valid_mask kernels as an expression node.
Expr expr_is_null(const Expr& a, bool null = true);

/// If `e` is `col <str pred> pattern`, fill the parts and return true. Lets a
/// source push a string predicate down as a query.
bool expr_as_col_str_pred(const Expr& e, std::int32_t* col, StrPredOp* op,
                          std::string_view* pattern);
/// If `e` is `col is_in values`, fill the parts and return true.
bool expr_as_col_is_in(const Expr& e, std::int32_t* col, Series* values);
Expr expr_cmp(CmpOp cmp, const Expr& a, Scalar rhs);
Expr expr_logical(LogicalOp op, const Expr& a, const Expr& b);
Expr expr_not(const Expr& a);
Expr expr_cast(TypeId type, const Expr& a);

/// Short aliases: reference input column `index`, or a literal.
inline Expr col(std::int32_t index) { return expr_col(index); }
inline Expr lit(std::int64_t value) { return expr_lit(value); }
inline Expr lit(double value) { return expr_lit(value); }

// Comparison operators take a scalar right-hand side (no Expr-vs-Expr compare).
inline Expr operator+(const Expr& a, const Expr& b) {
    return expr_binary(BinaryOp::Add, a, b);
}
inline Expr operator-(const Expr& a, const Expr& b) {
    return expr_binary(BinaryOp::Sub, a, b);
}
inline Expr operator*(const Expr& a, const Expr& b) {
    return expr_binary(BinaryOp::Mul, a, b);
}
inline Expr operator/(const Expr& a, const Expr& b) {
    return expr_binary(BinaryOp::Div, a, b);
}
inline Expr operator&(const Expr& a, const Expr& b) {
    return expr_logical(LogicalOp::And, a, b);
}
inline Expr operator|(const Expr& a, const Expr& b) {
    return expr_logical(LogicalOp::Or, a, b);
}
inline Expr operator~(const Expr& a) { return expr_not(a); }

namespace detail {
inline dftu_scalar expr_scalar_i(std::int64_t v) {
    dftu_scalar s;
    s.kind = DFTU_SCALAR_TAG_I64;
    s.value.i = v;
    return s;
}
inline dftu_scalar expr_scalar_d(double v) {
    dftu_scalar s;
    s.kind = DFTU_SCALAR_TAG_F64;
    s.value.d = v;
    return s;
}
}  // namespace detail

inline Expr operator>(const Expr& a, std::int64_t v) {
    return expr_cmp(CmpOp::Gt, a, detail::expr_scalar_i(v));
}
inline Expr operator>=(const Expr& a, std::int64_t v) {
    return expr_cmp(CmpOp::Ge, a, detail::expr_scalar_i(v));
}
inline Expr operator<(const Expr& a, std::int64_t v) {
    return expr_cmp(CmpOp::Lt, a, detail::expr_scalar_i(v));
}
inline Expr operator<=(const Expr& a, std::int64_t v) {
    return expr_cmp(CmpOp::Le, a, detail::expr_scalar_i(v));
}
inline Expr operator>(const Expr& a, double v) {
    return expr_cmp(CmpOp::Gt, a, detail::expr_scalar_d(v));
}
inline Expr operator>=(const Expr& a, double v) {
    return expr_cmp(CmpOp::Ge, a, detail::expr_scalar_d(v));
}
inline Expr operator<(const Expr& a, double v) {
    return expr_cmp(CmpOp::Lt, a, detail::expr_scalar_d(v));
}
inline Expr operator<=(const Expr& a, double v) {
    return expr_cmp(CmpOp::Le, a, detail::expr_scalar_d(v));
}

/// Compile `root` (type inference + CSE + lowering) and evaluate it over
/// `inputs` in one chunked pass. Throws std::invalid_argument on a malformed
/// expression or an out-of-range column reference.
Series eval(const Expr& root, const std::vector<const Series*>& inputs);

/// Compile `roots` into ONE slot program - CSE spans all of them, so a
/// subexpression shared across outputs (e.g. `a+b` in both `sum(a+b)` and
/// `var(a+b)`) is computed once - and evaluate them in a single chunked pass.
/// Only the input columns the program references are materialized (the pruner).
/// Returns one column per root, aligned to `roots`. Same throwing contract as
/// eval.
std::vector<Series> eval_many(const std::vector<Expr>& roots,
                              const std::vector<const Series*>& inputs);

/// The DataType `root` would produce over columns typed `input_types`,
/// without evaluating data. Shares eval()'s compiler, so the two can never
/// disagree. A bare column reference reports the input column's full
/// DataType (including timezone/decimal/fixed_size parameters); every other
/// form reports a scalar DataType, since no transform here can target a
/// parameterized type. Reports TypeId::Unknown where the input type is
/// itself Unknown. Same throwing contract as eval().
DataType infer_type(const Expr& root, const std::vector<DataType>& input_types);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_EXPR_H
