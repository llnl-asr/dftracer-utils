#include <dftracer/utils/dataframe/batch_ops.h>          // concat_columns
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/cell_ops.h>  // cell_to_string
#include <dftracer/utils/dataframe/internal/expr_handle.h>
#include <dftracer/utils/dataframe/internal/fingerprint.h>
#include <dftracer/utils/dataframe/internal/scalar.h>    // scalar_as
#include <dftracer/utils/dataframe/parallel.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace dataframe = dftracer::utils::dataframe;

namespace dftracer::utils::dataframe {

enum class ExprKind {
    LitI64,
    LitF64,
    Col,
    Binary,
    Prim,
    Unary,
    Clip,
    Fillna,
    Cmp,
    Logical,
    Not,
    Cast,
    StrPred,
    StrMap,
    StrLen,
    StrFind,
    StrReplace,
    StrSlice,
    IsIn,
    Select,
    IsNull
};

struct ExprNode {
    ExprKind kind;
    std::int32_t i = 0;     // col index / prim / unary / cmp op / logical op /
                            // cast type / binary op / str op / chars / all
    dftu_scalar scalar{};   // literal value / cmp rhs / clip lo / slice start
    dftu_scalar scalar2{};  // clip hi / slice len
    // Owns the text of a STR-tagged `scalar`, which borrows it, or a string
    // op's pattern / needle / from. The node is built once and never copied
    // (it lives behind shared_ptr<const>), and eval_many holds the roots for
    // the whole evaluation, so the borrow is valid for as long as the
    // compiled program can read it.
    std::string text;
    std::string text2;                  // str_replace `to`
    Series values;                      // is_in set
    std::shared_ptr<const ExprNode> a;  // first child (select: cond)
    std::shared_ptr<const ExprNode> b;  // second child (select: then)
    std::shared_ptr<const ExprNode> c;  // select: otherwise
};

namespace {

Expr make(ExprKind k, std::int32_t i, dftu_scalar s,
          std::shared_ptr<const ExprNode> a,
          std::shared_ptr<const ExprNode> b) {
    auto n = std::make_shared<ExprNode>();
    n->kind = k;
    n->i = i;
    n->scalar = s;
    n->a = std::move(a);
    n->b = std::move(b);
    return Expr{std::move(n)};
}

}  // namespace

Expr expr_col(std::int32_t index) {
    return make(ExprKind::Col, index, {}, nullptr, nullptr);
}
std::int32_t expr_col_index(const Expr& e) {
    const auto& n = e.node();
    return n && n->kind == ExprKind::Col ? n->i : -1;
}

namespace {

void fingerprint_node(detail::Fingerprint& fp, const ExprNode* n) {
    if (!n) {
        fp.pod(std::int32_t{-1});
        return;
    }
    fp.pod(static_cast<std::int32_t>(n->kind));
    fp.pod(n->i);
    fp.scalar(n->scalar);
    fp.scalar(n->scalar2);
    fp.str(n->text);
    fp.str(n->text2);
    const dftu_series* vals = n->values.handle();
    fp.pod(vals ? dftu_series_data(vals) : nullptr);
    fp.pod(vals ? n->values.length() : std::int64_t{0});
    fingerprint_node(fp, n->a.get());
    fingerprint_node(fp, n->b.get());
    fingerprint_node(fp, n->c.get());
}

}  // namespace

std::uint64_t expr_fingerprint(const Expr& e) {
    detail::Fingerprint fp;
    fingerprint_node(fp, e.node().get());
    return fp.value();
}

bool expr_as_col_cmp(const Expr& e, std::int32_t* col, CmpOp* op, Scalar* rhs) {
    const auto& n = e.node();
    if (!n || n->kind != ExprKind::Cmp || !n->a || n->a->kind != ExprKind::Col)
        return false;
    *col = n->a->i;
    *op = static_cast<CmpOp>(n->i);
    *rhs = n->scalar;
    return true;
}

bool expr_as_col_binary(const Expr& e, BinaryOp* op, std::int32_t* a,
                        std::int32_t* b) {
    const auto& n = e.node();
    if (!n || n->kind != ExprKind::Binary || !n->a || !n->b ||
        n->a->kind != ExprKind::Col || n->b->kind != ExprKind::Col)
        return false;
    *op = static_cast<BinaryOp>(n->i);
    *a = n->a->i;
    *b = n->b->i;
    return true;
}

bool expr_as_logical(const Expr& e, LogicalOp* op, Expr* a, Expr* b) {
    const auto& n = e.node();
    if (!n || n->kind != ExprKind::Logical || !n->a || !n->b) return false;
    *op = static_cast<LogicalOp>(n->i);
    *a = Expr{n->a};
    *b = Expr{n->b};
    return true;
}

bool expr_as_not(const Expr& e, Expr* a) {
    const auto& n = e.node();
    if (!n || n->kind != ExprKind::Not || !n->a) return false;
    *a = Expr{n->a};
    return true;
}

namespace {
bool node_references(const std::shared_ptr<const ExprNode>& n,
                     std::int32_t index) {
    if (!n) return false;
    if (n->kind == ExprKind::Col) return n->i == index;
    return node_references(n->a, index) || node_references(n->b, index) ||
           node_references(n->c, index);
}
}  // namespace

bool expr_references(const Expr& e, std::int32_t index) {
    return node_references(e.node(), index);
}

namespace {
std::int32_t node_max_col(const std::shared_ptr<const ExprNode>& n) {
    if (!n) return -1;
    if (n->kind == ExprKind::Col) return n->i;
    return std::max(
        {node_max_col(n->a), node_max_col(n->b), node_max_col(n->c)});
}
}  // namespace

std::int32_t expr_max_col(const Expr& e) { return node_max_col(e.node()); }

namespace {
// Series is move-only, so a node copy shares the values set explicitly.
std::shared_ptr<ExprNode> clone(const ExprNode& n) {
    auto c = std::make_shared<ExprNode>();
    c->kind = n.kind;
    c->i = n.i;
    c->scalar = n.scalar;
    c->scalar2 = n.scalar2;
    c->text = n.text;
    c->text2 = n.text2;
    if (n.scalar.kind == DFTU_SCALAR_TAG_STR) {
        c->scalar.value.s = c->text.data();
        c->scalar.len = static_cast<std::uint32_t>(c->text.size());
    }
    if (n.values.valid()) c->values = n.values.share();
    c->a = n.a;
    c->b = n.b;
    c->c = n.c;
    return c;
}

std::shared_ptr<const ExprNode> node_remap(
    const std::shared_ptr<const ExprNode>& n,
    const std::vector<std::int32_t>& old_to_new) {
    if (!n) return nullptr;
    if (n->kind == ExprKind::Col) {
        const bool in_range =
            n->i >= 0 && static_cast<std::size_t>(n->i) < old_to_new.size();
        auto c = clone(*n);
        c->i = in_range ? old_to_new[static_cast<std::size_t>(n->i)] : n->i;
        return c;
    }
    auto a = node_remap(n->a, old_to_new);
    auto b = node_remap(n->b, old_to_new);
    auto c3 = node_remap(n->c, old_to_new);
    if (a == n->a && b == n->b && c3 == n->c) return n;  // unchanged: share
    auto c = clone(*n);
    c->a = std::move(a);
    c->b = std::move(b);
    c->c = std::move(c3);
    return c;
}
std::shared_ptr<const ExprNode> node_rebind(
    const std::shared_ptr<const ExprNode>& n, const std::vector<Expr>& by) {
    if (!n) return nullptr;
    if (n->kind == ExprKind::Col) {
        const bool in_range =
            n->i >= 0 && static_cast<std::size_t>(n->i) < by.size();
        return in_range && by[static_cast<std::size_t>(n->i)].valid()
                   ? by[static_cast<std::size_t>(n->i)].node()
                   : n;
    }
    auto a = node_rebind(n->a, by);
    auto b = node_rebind(n->b, by);
    auto c3 = node_rebind(n->c, by);
    if (a == n->a && b == n->b && c3 == n->c) return n;
    auto c = clone(*n);
    c->a = std::move(a);
    c->b = std::move(b);
    c->c = std::move(c3);
    return c;
}

void canonical_scalar(std::string& out, const dftu_scalar& v) {
    out += std::to_string(v.kind);
    out += ':';
    if (v.kind == DFTU_SCALAR_TAG_STR) {
        const std::string_view s =
            v.value.s ? std::string_view(v.value.s, v.len) : std::string_view();
        out += std::to_string(s.size());
        out += ':';
        out += s;
    } else {
        out += std::to_string(v.value.u);
    }
}

void canonical_text(std::string& out, std::string_view s) {
    out += std::to_string(s.size());
    out += ':';
    out += s;
}

void canonical_node(std::string& out, const ExprNode* n) {
    if (!n) {
        out += '_';
        return;
    }
    out += '(';
    out += std::to_string(static_cast<int>(n->kind));
    out += ',';
    out += std::to_string(n->i);
    out += ',';
    canonical_scalar(out, n->scalar);
    out += ',';
    canonical_scalar(out, n->scalar2);
    out += ',';
    canonical_text(out, n->text);
    out += ',';
    canonical_text(out, n->text2);
    out += ',';
    if (n->values.valid()) {
        out += std::to_string(static_cast<int>(n->values.type()));
        for (std::int64_t r = 0; r < n->values.length(); ++r) {
            out += ',';
            canonical_text(out, cell_to_string(n->values, r));
        }
    }
    out += ',';
    canonical_node(out, n->a.get());
    out += ',';
    canonical_node(out, n->b.get());
    out += ',';
    canonical_node(out, n->c.get());
    out += ')';
}

}  // namespace

Expr expr_remap_cols(const Expr& e,
                     const std::vector<std::int32_t>& old_to_new) {
    return Expr{node_remap(e.node(), old_to_new)};
}

Expr expr_rebind_cols(const Expr& e, const std::vector<Expr>& by_index) {
    return Expr{node_rebind(e.node(), by_index)};
}

std::string expr_canonical(const Expr& e) {
    std::string out;
    canonical_node(out, e.node().get());
    return out;
}
Expr expr_lit(std::int64_t value) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_I64;
    s.value.i = value;
    return make(ExprKind::LitI64, 0, s, nullptr, nullptr);
}
Expr expr_lit(double value) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_F64;
    s.value.d = value;
    return make(ExprKind::LitF64, 0, s, nullptr, nullptr);
}
Expr expr_binary(BinaryOp op, const Expr& a, const Expr& b) {
    return make(ExprKind::Binary, static_cast<std::int32_t>(op), {}, a.node(),
                b.node());
}
Expr expr_prim(PrimOp prim, const Expr& a) {
    return make(ExprKind::Prim, static_cast<std::int32_t>(prim), {}, a.node(),
                nullptr);
}
Expr expr_unary(UnaryOp op, const Expr& a) {
    return make(ExprKind::Unary, static_cast<std::int32_t>(op), {}, a.node(),
                nullptr);
}
Expr expr_clip(const Expr& a, Scalar lo, Scalar hi) {
    auto n = std::make_shared<ExprNode>();
    n->kind = ExprKind::Clip;
    n->scalar = lo;
    n->scalar2 = hi;
    n->a = a.node();
    return Expr{std::move(n)};
}
Expr expr_fillna(const Expr& a, Scalar fill) {
    return make(ExprKind::Fillna, 0, fill, a.node(), nullptr);
}
Expr expr_cmp(CmpOp cmp, const Expr& a, Scalar rhs) {
    Expr e = make(ExprKind::Cmp, static_cast<std::int32_t>(cmp), rhs, a.node(),
                  nullptr);
    // A STR rhs only borrows its text, and the caller's buffer may die before
    // the Expr is evaluated. Take a copy the node owns and repoint at it.
    const dftu_scalar raw = rhs;
    if (raw.kind == DFTU_SCALAR_TAG_STR) {
        auto* n = const_cast<ExprNode*>(e.node().get());
        n->text.assign(raw.value.s != nullptr ? raw.value.s : "", raw.len);
        n->scalar.value.s = n->text.data();
        n->scalar.len = static_cast<std::uint32_t>(n->text.size());
    }
    return e;
}
Expr expr_logical(LogicalOp op, const Expr& a, const Expr& b) {
    return make(ExprKind::Logical, static_cast<std::int32_t>(op), {}, a.node(),
                b.node());
}
Expr expr_not(const Expr& a) {
    return make(ExprKind::Not, 0, {}, a.node(), nullptr);
}
Expr expr_cast(TypeId type, const Expr& a) {
    return make(ExprKind::Cast, static_cast<std::int32_t>(type), {}, a.node(),
                nullptr);
}
Expr expr_str_pred(StrPredOp op, const Expr& a, std::string_view pattern) {
    Expr e = make(ExprKind::StrPred, static_cast<std::int32_t>(op), {},
                  a.node(), nullptr);
    const_cast<ExprNode*>(e.node().get())->text.assign(pattern);
    return e;
}
Expr expr_str_map(StrMapOp op, const Expr& a) {
    return make(ExprKind::StrMap, static_cast<std::int32_t>(op), {}, a.node(),
                nullptr);
}
Expr expr_lower(const Expr& a) { return expr_str_map(StrMapOp::Lower, a); }
Expr expr_str_len(const Expr& a, bool chars) {
    return make(ExprKind::StrLen, chars ? 1 : 0, {}, a.node(), nullptr);
}
Expr expr_str_find(const Expr& a, std::string_view needle) {
    Expr e = make(ExprKind::StrFind, 0, {}, a.node(), nullptr);
    const_cast<ExprNode*>(e.node().get())->text.assign(needle);
    return e;
}
Expr expr_str_replace(const Expr& a, std::string_view from, std::string_view to,
                      bool all) {
    Expr e = make(ExprKind::StrReplace, all ? 1 : 0, {}, a.node(), nullptr);
    auto* n = const_cast<ExprNode*>(e.node().get());
    n->text.assign(from);
    n->text2.assign(to);
    return e;
}
Expr expr_str_slice(const Expr& a, std::int64_t start, std::int64_t len) {
    auto n = std::make_shared<ExprNode>();
    n->kind = ExprKind::StrSlice;
    n->scalar.kind = DFTU_SCALAR_TAG_I64;
    n->scalar.value.i = start;
    n->scalar2.kind = DFTU_SCALAR_TAG_I64;
    n->scalar2.value.i = len;
    n->a = a.node();
    return Expr{std::move(n)};
}
Expr expr_is_in(const Expr& a, Series values) {
    Expr e = make(ExprKind::IsIn, 0, {}, a.node(), nullptr);
    const_cast<ExprNode*>(e.node().get())->values = std::move(values);
    return e;
}
Expr expr_is_null(const Expr& a, bool null) {
    return make(ExprKind::IsNull, null ? 1 : 0, {}, a.node(), nullptr);
}
Expr expr_select(const Expr& cond, const Expr& a, const Expr& b) {
    Expr e = make(ExprKind::Select, 0, {}, cond.node(), a.node());
    const_cast<ExprNode*>(e.node().get())->c = b.node();
    return e;
}

bool expr_as_col_str_pred(const Expr& e, std::int32_t* col, StrPredOp* op,
                          std::string_view* pattern) {
    const auto& n = e.node();
    if (!n || n->kind != ExprKind::StrPred || !n->a ||
        n->a->kind != ExprKind::Col)
        return false;
    *col = n->a->i;
    *op = static_cast<StrPredOp>(n->i);
    *pattern = n->text;
    return true;
}

bool expr_as_col_is_in(const Expr& e, std::int32_t* col, Series* values) {
    const auto& n = e.node();
    if (!n || n->kind != ExprKind::IsIn || !n->a || n->a->kind != ExprKind::Col)
        return false;
    *col = n->a->i;
    *values = n->values.share();
    return true;
}

// ---- compiler: type inference + CSE + lowering to a slot program ----------

namespace {

// Slot-IR opcodes.
enum {
    OP_LOAD,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_ADDS,
    OP_SUBS,
    OP_MULS,
    OP_DIVS,
    OP_PRIM,
    OP_UNARY,
    OP_CLIP,
    OP_FILLNA,
    OP_CMP,
    OP_LOGICAL,
    OP_NOT,
    OP_CAST,
    OP_STR_PRED,
    OP_STR_MAP,
    OP_STR_LEN,
    OP_STR_FIND,
    OP_STR_REPLACE,
    OP_STR_SLICE,
    OP_IS_IN,
    OP_CONST,   // a column of `param` type filled with `scalar`
    OP_SELECT,  // c ? a : b, with the mask in slot `c`
    OP_IS_NULL  // the null mask of slot `a` (param 1) or the valid mask (0)
};

struct SlotOp {
    int opcode = 0;
    int a = -1;
    int b = -1;
    int c = -1;
    std::int32_t param = 0;
    dftu_scalar scalar{};
    dftu_scalar scalar2{};  // clip hi / slice len
    // Borrowed from the ExprNode that emitted the op, which outlives the
    // program (eval_many holds the roots).
    std::string_view text;
    std::string_view text2;
    const dftu_series* values = nullptr;
};

const int COL_OP[4] = {OP_ADD, OP_SUB, OP_MUL, OP_DIV};
const int SCALAR_OP[4] = {OP_ADDS, OP_SUBS, OP_MULS, OP_DIVS};

// A compiled subexpression: either a compile-time scalar or a column in slot
// `slot` of type `type`. `full` is the complete DataType: a bare column
// reference passes its input DataType through, every other node reports a
// scalar DataType (no kernel here can target a parameterized type).
struct Val {
    bool is_scalar;
    int slot;
    TypeId type;
    dftu_scalar scalar;
    DataType full;
};

bool is_float(const Val& v) {
    if (v.is_scalar) return v.scalar.kind == DFTU_SCALAR_TAG_F64;
    return v.type == TypeId::Float32 || v.type == TypeId::Float64;
}

bool is_unknown(const Val& v) { return v.full.id == TypeId::Unknown; }
Val unknown_val() {
    return {false, -1, TypeId::Unknown, {}, scalar(TypeId::Unknown)};
}

double scalar_to_double(dftu_scalar s) {
    if (s.kind == DFTU_SCALAR_TAG_I64) return static_cast<double>(s.value.i);
    if (s.kind == DFTU_SCALAR_TAG_U64) return static_cast<double>(s.value.u);
    return s.value.d;
}

dftu_scalar to_f64_scalar(dftu_scalar s) {
    dftu_scalar r{};
    r.kind = DFTU_SCALAR_TAG_F64;
    r.value.d = scalar_to_double(s);
    return r;
}

// Depends only on each input column's DataType, never its data, so the same
// compile() drives eval() (Series::data_type()) and infer_type() (a schema
// with no data) and the two can never disagree.
class Compiler {
   public:
    explicit Compiler(const std::vector<DataType>& input_types)
        : input_types_(input_types) {}

    Val compile(const ExprNode* n) {
        switch (n->kind) {
            case ExprKind::LitI64:
            case ExprKind::LitF64:
                return {true, -1, TypeId::Int64, n->scalar,
                        scalar(TypeId::Int64)};
            case ExprKind::Col: {
                if (n->i < 0 ||
                    static_cast<std::size_t>(n->i) >= input_types_.size())
                    throw std::invalid_argument(
                        "expr: column index out of range");
                const DataType& dt =
                    input_types_[static_cast<std::size_t>(n->i)];
                if (dt.id == TypeId::Unknown)
                    return {false, -1, TypeId::Unknown, {}, dt};
                int slot = emit(OP_LOAD, -1, -1, n->i, {});
                return {false, slot, dt.id, {}, dt};
            }
            case ExprKind::Binary:
                return compile_binary(n);
            case ExprKind::Prim: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_col(a0, "prim");
                if (a.type != TypeId::Int64 && a.type != TypeId::Uint64)
                    a = cast(a, TypeId::Int64);
                return {false,
                        emit(OP_PRIM, a.slot, -1, n->i, {}),
                        TypeId::Int64,
                        {},
                        scalar(TypeId::Int64)};
            }
            case ExprKind::Unary: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_col(a0, "unary");
                // is_nan/is_finite/is_infinite yield a Bool mask; log/sqrt/exp
                // widen to Float64; the rest keep the input type (integer
                // floor/ceil/round/trunc are identities).
                const auto op = static_cast<UnaryOp>(n->i);
                TypeId t;
                switch (op) {
                    case UnaryOp::IsNan:
                    case UnaryOp::IsFinite:
                    case UnaryOp::IsInfinite:
                        t = TypeId::Bool;
                        break;
                    case UnaryOp::Log:
                    case UnaryOp::Sqrt:
                    case UnaryOp::Exp:
                        t = TypeId::Float64;
                        break;
                    default:
                        t = a.type;
                        break;
                }
                return {false,
                        emit(OP_UNARY, a.slot, -1, n->i, {}),
                        t,
                        {},
                        scalar(t)};
            }
            case ExprKind::Clip: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_col(a0, "clip");
                return {false,
                        emit(OP_CLIP, a.slot, -1, 0, n->scalar, n->scalar2),
                        a.type,
                        {},
                        scalar(a.type)};
            }
            case ExprKind::Fillna: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_col(a0, "fillna");
                return {false,
                        emit(OP_FILLNA, a.slot, -1, 0, n->scalar),
                        a.type,
                        {},
                        scalar(a.type)};
            }
            case ExprKind::Cmp: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_col(a0, "compare");
                return {false,
                        emit(OP_CMP, a.slot, -1, n->i, n->scalar),
                        TypeId::Bool,
                        {},
                        scalar(TypeId::Bool)};
            }
            case ExprKind::Logical: {
                Val a0 = compile(n->a.get());
                Val b0 = compile(n->b.get());
                if (is_unknown(a0) || is_unknown(b0)) return unknown_val();
                Val a = as_col(a0, "logical");
                Val b = as_col(b0, "logical");
                return {false,
                        emit(OP_LOGICAL, a.slot, b.slot, n->i, {}),
                        TypeId::Bool,
                        {},
                        scalar(TypeId::Bool)};
            }
            case ExprKind::Not: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_col(a0, "not");
                return {false,
                        emit(OP_NOT, a.slot, -1, 0, {}),
                        TypeId::Bool,
                        {},
                        scalar(TypeId::Bool)};
            }
            case ExprKind::Cast: {
                Val a = as_col(compile(n->a.get()), "cast");
                return cast(a, static_cast<TypeId>(n->i));
            }
            case ExprKind::StrPred: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_str(a0, "string predicate", true);
                return {false,
                        emit_text(OP_STR_PRED, a.slot, n->i, n->text),
                        TypeId::Bool,
                        {},
                        scalar(TypeId::Bool)};
            }
            case ExprKind::StrMap: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_str(a0, "string map", false);
                return {false,
                        emit(OP_STR_MAP, a.slot, -1, n->i, {}),
                        TypeId::String,
                        {},
                        scalar(TypeId::String)};
            }
            case ExprKind::StrLen: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_str(a0, "string length", true);
                return {false,
                        emit(OP_STR_LEN, a.slot, -1, n->i, {}),
                        TypeId::Int64,
                        {},
                        scalar(TypeId::Int64)};
            }
            case ExprKind::StrFind: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_str(a0, "string find", true);
                return {false,
                        emit_text(OP_STR_FIND, a.slot, 0, n->text),
                        TypeId::Int64,
                        {},
                        scalar(TypeId::Int64)};
            }
            case ExprKind::StrReplace: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_str(a0, "string replace", false);
                return {
                    false,
                    emit_text(OP_STR_REPLACE, a.slot, n->i, n->text, n->text2),
                    TypeId::String,
                    {},
                    scalar(TypeId::String)};
            }
            case ExprKind::StrSlice: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_str(a0, "string slice", false);
                return {
                    false,
                    emit(OP_STR_SLICE, a.slot, -1, 0, n->scalar, n->scalar2),
                    TypeId::String,
                    {},
                    scalar(TypeId::String)};
            }
            case ExprKind::IsIn: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_col(a0, "is_in");
                if (!n->values.valid())
                    throw std::invalid_argument(
                        "expr: is_in needs a values column");
                if (value_domain(a.type) != value_domain(n->values.type()))
                    throw std::invalid_argument(
                        std::string("expr: is_in over a ") + type_name(a.type) +
                        " column needs " + type_name(a.type) + " values, got " +
                        type_name(n->values.type()));
                SlotOp op;
                op.opcode = OP_IS_IN;
                op.a = a.slot;
                op.values = n->values.handle();
                return {
                    false, emit_op(op), TypeId::Bool, {}, scalar(TypeId::Bool)};
            }
            case ExprKind::Select:
                return compile_select(n);
            case ExprKind::IsNull: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_col(a0, "is_null");
                return {false,
                        emit(OP_IS_NULL, a.slot, -1, n->i, {}),
                        TypeId::Bool,
                        {},
                        scalar(TypeId::Bool)};
            }
        }
        throw std::invalid_argument("expr: unknown node");
    }

    std::vector<SlotOp> program;

   private:
    Val as_col(Val v, const char* who) {
        if (v.is_scalar)
            throw std::invalid_argument(std::string("expr: ") + who +
                                        " needs a column operand");
        return v;
    }

    // A string operand: String always; Binary too when `binary_ok` (the
    // predicate / length / find kernels read bytes, the maps produce text).
    Val as_str(Val v, const char* who, bool binary_ok) {
        Val a = as_col(v, who);
        const bool ok =
            a.type == TypeId::String || (binary_ok && a.type == TypeId::Binary);
        if (!ok)
            throw std::invalid_argument(std::string("expr: ") + who +
                                        " needs a String" +
                                        (binary_ok ? " or Binary" : "") +
                                        " column, got " + type_name(a.type));
        return a;
    }

    Val cast(Val v, TypeId t) {
        return {false,
                emit(OP_CAST, v.slot, -1, static_cast<std::int32_t>(t), {}),
                t,
                {},
                scalar(t)};
    }

    Val compile_binary(const ExprNode* n) {
        const int op = n->i;  // BinaryOp
        Val a = compile(n->a.get());
        Val b = compile(n->b.get());
        if (is_unknown(a) || is_unknown(b)) return unknown_val();
        if (a.is_scalar && b.is_scalar) return fold(op, a.scalar, b.scalar);

        const bool rf =
            op == static_cast<int>(BinaryOp::Div) || is_float(a) || is_float(b);
        TypeId out_type;
        if (rf) {
            promote_float(a);
            promote_float(b);
            out_type = TypeId::Float64;
        } else if (!a.is_scalar && !b.is_scalar && a.type != b.type) {
            a = cast(a, TypeId::Int64);
            b = cast(b, TypeId::Int64);
            out_type = TypeId::Int64;
        } else {
            out_type = a.is_scalar ? b.type : a.type;
        }

        if (!a.is_scalar && !b.is_scalar)
            return {false,
                    emit(COL_OP[op], a.slot, b.slot, 0, {}),
                    out_type,
                    {},
                    scalar(out_type)};
        if (!a.is_scalar)  // col op scalar
            return {false,
                    emit(SCALAR_OP[op], a.slot, -1, 0, b.scalar),
                    out_type,
                    {},
                    scalar(out_type)};
        if (op == static_cast<int>(BinaryOp::Add) ||
            op == static_cast<int>(BinaryOp::Mul))  // scalar op col commutes
            return {false,
                    emit(SCALAR_OP[op], b.slot, -1, 0, a.scalar),
                    out_type,
                    {},
                    scalar(out_type)};
        throw std::invalid_argument("expr: scalar - / column has no kernel");
    }

    void promote_float(Val& v) {
        if (v.is_scalar) {
            v.scalar = to_f64_scalar(v.scalar);
        } else if (v.type != TypeId::Float32 && v.type != TypeId::Float64) {
            v = cast(v, TypeId::Float64);
        }
    }

    // A column of `t` holding `s` in every row, for a scalar arm of a select.
    Val broadcast(dftu_scalar s, TypeId t) {
        return {false,
                emit(OP_CONST, -1, -1, static_cast<std::int32_t>(t), s),
                t,
                {},
                scalar(t)};
    }

    // cond ? a : b. The arms promote as arithmetic does: either float makes
    // both Float64, two different integer types meet at Int64; a scalar arm
    // broadcasts into a column of the promoted type. A string arm needs a
    // string on the other side too.
    Val compile_select(const ExprNode* n) {
        Val c0 = compile(n->a.get());
        Val a = compile(n->b.get());
        Val b = compile(n->c.get());
        if (is_unknown(c0) || is_unknown(a) || is_unknown(b))
            return unknown_val();
        Val c = as_col(c0, "select");
        if (c.type != TypeId::Bool)
            throw std::invalid_argument(
                std::string("expr: select needs a Bool condition, got ") +
                type_name(c.type));
        if (a.is_scalar && b.is_scalar) {
            const bool f = a.scalar.kind == DFTU_SCALAR_TAG_F64 ||
                           b.scalar.kind == DFTU_SCALAR_TAG_F64;
            const TypeId t = f ? TypeId::Float64 : TypeId::Int64;
            if (f) {
                a.scalar = to_f64_scalar(a.scalar);
                b.scalar = to_f64_scalar(b.scalar);
            }
            a = broadcast(a.scalar, t);
            b = broadcast(b.scalar, t);
        } else {
            const bool str_arm = (!a.is_scalar && a.type == TypeId::String) ||
                                 (!b.is_scalar && b.type == TypeId::String);
            if (str_arm) {
                if (a.is_scalar || b.is_scalar || a.type != b.type)
                    throw std::invalid_argument(
                        "expr: select needs two String columns for a "
                        "String arm");
            } else if (is_float(a) || is_float(b)) {
                promote_float(a);
                promote_float(b);
                if (a.is_scalar) a = broadcast(a.scalar, TypeId::Float64);
                if (b.is_scalar) b = broadcast(b.scalar, TypeId::Float64);
            } else {
                const TypeId t = a.is_scalar        ? b.type
                                 : b.is_scalar      ? a.type
                                 : a.type == b.type ? a.type
                                                    : TypeId::Int64;
                if (!a.is_scalar && a.type != t) a = cast(a, t);
                if (!b.is_scalar && b.type != t) b = cast(b, t);
                if (a.is_scalar) a = broadcast(a.scalar, t);
                if (b.is_scalar) b = broadcast(b.scalar, t);
            }
        }
        SlotOp op;
        op.opcode = OP_SELECT;
        op.a = a.slot;
        op.b = b.slot;
        op.c = c.slot;
        return {false, emit_op(op), a.type, {}, scalar(a.type)};
    }

    Val fold(int op, dftu_scalar a, dftu_scalar b) {
        const bool f = op == static_cast<int>(BinaryOp::Div) ||
                       a.kind == DFTU_SCALAR_TAG_F64 ||
                       b.kind == DFTU_SCALAR_TAG_F64;
        dftu_scalar r{};
        if (f) {
            double x = scalar_to_double(a), y = scalar_to_double(b);
            r.kind = DFTU_SCALAR_TAG_F64;
            r.value.d = op == 0   ? x + y
                        : op == 1 ? x - y
                        : op == 2 ? x * y
                                  : x / y;
        } else {
            std::int64_t x = a.value.i, y = b.value.i;
            r.kind = DFTU_SCALAR_TAG_I64;
            r.value.i = op == 0   ? x + y
                        : op == 1 ? x - y
                        : op == 2 ? x * y
                                  : (y != 0 ? x / y : 0);
        }
        return {true, -1, TypeId::Int64, r,
                scalar(f ? TypeId::Float64 : TypeId::Int64)};
    }

    // Emit an op, hash-consing structurally identical ops to the same slot
    // (common-subexpression elimination).
    int emit(int opcode, int a, int b, std::int32_t param, dftu_scalar s,
             dftu_scalar s2 = {}) {
        SlotOp op;
        op.opcode = opcode;
        op.a = a;
        op.b = b;
        op.param = param;
        op.scalar = s;
        op.scalar2 = s2;
        return emit_op(op);
    }

    int emit_text(int opcode, int a, std::int32_t param, std::string_view text,
                  std::string_view text2 = {}) {
        SlotOp op;
        op.opcode = opcode;
        op.a = a;
        op.param = param;
        op.text = text;
        op.text2 = text2;
        return emit_op(op);
    }

    int emit_op(const SlotOp& op) {
        auto bits = [](dftu_scalar x) {
            return x.kind == DFTU_SCALAR_TAG_F64
                       ? std::bit_cast<std::int64_t>(x.value.d)
                       : x.value.i;
        };
        // len is part of the key: a STR scalar's `bits` is its pointer, and
        // two scalars can share a pointer with different lengths (a prefix),
        // which would otherwise hash-cons to the same slot. A text operand is
        // keyed by value, a values set by identity.
        auto key = std::make_tuple(
            op.opcode, op.a, op.b, op.c, static_cast<int>(op.param),
            static_cast<int>(op.scalar.kind), bits(op.scalar), bits(op.scalar2),
            op.scalar.len, op.scalar2.len, std::string(op.text),
            std::string(op.text2), reinterpret_cast<std::uintptr_t>(op.values));
        auto it = memo_.find(key);
        if (it != memo_.end()) return it->second;
        int slot = static_cast<int>(program.size());
        program.push_back(op);
        memo_.emplace(key, slot);
        return slot;
    }

    const std::vector<DataType>& input_types_;
    std::map<std::tuple<int, int, int, int, int, int, std::int64_t,
                        std::int64_t, std::uint32_t, std::uint32_t, std::string,
                        std::string, std::uintptr_t>,
             int>
        memo_;
};

// dftu_series_slice is FLAT-fixed-width only, so it returns null for
// String/Binary/List; take()'s gather_column handles those instead.
Series load_slice(const Series& in, std::int64_t offset, std::int64_t len) {
    const TypeId t = in.type();
    if (byte_width(t, in.data_type().fixed_size)) return in.slice(offset, len);
    std::vector<std::int64_t> idx(static_cast<std::size_t>(len));
    for (std::int64_t i = 0; i < len; ++i)
        idx[static_cast<std::size_t>(i)] = offset + i;
    return in.take(idx);
}

template <class T>
Series const_fixed(TypeId t, const dftu_scalar& s, std::int64_t len) {
    std::vector<T> v(static_cast<std::size_t>(len), scalar_as<T>(s));
    return Series::flat(t, v.data(), len);
}

// A column of `len` rows of type `t`, every row holding `s` (a select arm
// given as a literal). Refuses a type with no scalar form rather than build a
// column whose contents would be a guess.
Series const_column(TypeId t, const dftu_scalar& s, std::int64_t len) {
    switch (t) {
        case TypeId::Bool: {
            const bool on = scalar_as<std::int64_t>(s) != 0;
            std::vector<std::uint8_t> bits(buffer_bytes(TypeId::Bool, len),
                                           on ? 0xFF : 0x00);
            return Series::flat(t, bits.data(), len);
        }
        case TypeId::Int8:
            return const_fixed<std::int8_t>(t, s, len);
        case TypeId::Int16:
            return const_fixed<std::int16_t>(t, s, len);
        case TypeId::Int32:
            return const_fixed<std::int32_t>(t, s, len);
        case TypeId::Int64:
            return const_fixed<std::int64_t>(t, s, len);
        case TypeId::Uint8:
            return const_fixed<std::uint8_t>(t, s, len);
        case TypeId::Uint16:
            return const_fixed<std::uint16_t>(t, s, len);
        case TypeId::Uint32:
            return const_fixed<std::uint32_t>(t, s, len);
        case TypeId::Uint64:
            return const_fixed<std::uint64_t>(t, s, len);
        case TypeId::Float32:
            return const_fixed<float>(t, s, len);
        case TypeId::Float64:
            return const_fixed<double>(t, s, len);
        case TypeId::Unknown:
        case TypeId::Float16:
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::List:
        case TypeId::Struct:
        case TypeId::Date32:
        case TypeId::Date64:
        case TypeId::Time32:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
        case TypeId::Decimal128:
        case TypeId::Decimal256:
        case TypeId::FixedSizeBinary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Map:
            break;
    }
    throw std::invalid_argument(
        std::string("expr: a literal cannot broadcast to a ") + type_name(t) +
        " column");
}

// Evaluate the slot program over rows [offset, offset+len) and extract one
// column per requested final slot (shared, so distinct outputs that resolved to
// the same slot alias the one buffer).
std::vector<Series> eval_chunk(const std::vector<SlotOp>& prog,
                               const std::vector<Series>& inputs,
                               const std::vector<int>& finals,
                               std::int64_t offset, std::int64_t len) {
    std::vector<Series> s(prog.size());
    for (std::size_t k = 0; k < prog.size(); ++k) {
        const SlotOp& op = prog[k];
        auto A = [&]() { return s[static_cast<std::size_t>(op.a)].handle(); };
        auto B = [&]() { return s[static_cast<std::size_t>(op.b)].handle(); };
        switch (op.opcode) {
            case OP_LOAD:
                s[k] = load_slice(inputs[static_cast<std::size_t>(op.param)],
                                  offset, len);
                break;
            case OP_ADD:
                s[k] = Series{dftu_series_add(A(), B())};
                break;
            case OP_SUB:
                s[k] = Series{dftu_series_sub(A(), B())};
                break;
            case OP_MUL:
                s[k] = Series{dftu_series_mul(A(), B())};
                break;
            case OP_DIV:
                s[k] = Series{dftu_series_div(A(), B())};
                break;
            case OP_ADDS:
                s[k] = Series{dftu_series_add_scalar(A(), op.scalar)};
                break;
            case OP_SUBS:
                s[k] = Series{dftu_series_sub_scalar(A(), op.scalar)};
                break;
            case OP_MULS:
                s[k] = Series{dftu_series_mul_scalar(A(), op.scalar)};
                break;
            case OP_DIVS:
                s[k] = Series{dftu_series_div_scalar(A(), op.scalar)};
                break;
            case OP_PRIM:
                s[k] = Series{
                    dftu_series_prim(A(), static_cast<dftu_prim_op>(op.param))};
                break;
            case OP_UNARY: {
                dftu_series* r = nullptr;
                switch (static_cast<UnaryOp>(op.param)) {
                    case UnaryOp::Abs:
                        r = dftu_series_abs(A());
                        break;
                    case UnaryOp::Round:
                        r = dftu_series_round(A());
                        break;
                    case UnaryOp::Floor:
                        r = dftu_series_floor(A());
                        break;
                    case UnaryOp::Ceil:
                        r = dftu_series_ceil(A());
                        break;
                    case UnaryOp::Log:
                        r = dftu_series_log(A());
                        break;
                    case UnaryOp::Sqrt:
                        r = dftu_series_sqrt(A());
                        break;
                    case UnaryOp::Exp:
                        r = dftu_series_exp(A());
                        break;
                    case UnaryOp::Sign:
                        r = dftu_series_sign(A());
                        break;
                    case UnaryOp::Negate:
                        r = dftu_series_negate(A());
                        break;
                    case UnaryOp::Trunc:
                        r = dftu_series_trunc(A());
                        break;
                    case UnaryOp::IsNan:
                        r = dftu_series_is_nan(A());
                        break;
                    case UnaryOp::IsFinite:
                        r = dftu_series_is_finite(A());
                        break;
                    case UnaryOp::IsInfinite:
                        r = dftu_series_is_infinite(A());
                        break;
                }
                s[k] = Series{r};
                break;
            }
            case OP_CLIP:
                s[k] = Series{dftu_series_clip(A(), op.scalar, op.scalar2)};
                break;
            case OP_FILLNA:
                s[k] = Series{dftu_series_fillna(A(), op.scalar)};
                break;
            case OP_CMP:
                s[k] = Series{dftu_series_compare(
                    A(), static_cast<dftu_cmp_op>(op.param), op.scalar)};
                break;
            case OP_LOGICAL:
                s[k] = Series{dftu_series_logical(
                    A(), B(), static_cast<dftu_logical_op>(op.param))};
                break;
            case OP_NOT:
                s[k] = Series{dftu_series_logical_not(A())};
                break;
            case OP_CAST:
                s[k] = Series{
                    dftu_series_cast(A(), static_cast<dftu_dtype>(op.param))};
                break;
            case OP_STR_PRED: {
                const char* p = op.text.data();
                const auto n = static_cast<std::int32_t>(op.text.size());
                dftu_series* r = nullptr;
                switch (static_cast<StrPredOp>(op.param)) {
                    case StrPredOp::Contains:
                        r = dftu_series_str_contains(A(), p, n);
                        break;
                    case StrPredOp::StartsWith:
                        r = dftu_series_str_starts_with(A(), p, n);
                        break;
                    case StrPredOp::EndsWith:
                        r = dftu_series_str_ends_with(A(), p, n);
                        break;
                    case StrPredOp::Like:
                        r = dftu_series_str_like(A(), p, n);
                        break;
                    case StrPredOp::Matches:
                        r = dftu_series_str_matches(A(), p, n);
                        break;
                    case StrPredOp::Search:
                        r = dftu_series_str_search(A(), p, n);
                        break;
                }
                s[k] = Series{r};
                break;
            }
            case OP_STR_MAP: {
                dftu_series* r = nullptr;
                switch (static_cast<StrMapOp>(op.param)) {
                    case StrMapOp::Lower:
                        r = dftu_series_to_lowercase(A());
                        break;
                    case StrMapOp::Upper:
                        r = dftu_series_to_uppercase(A());
                        break;
                    case StrMapOp::Strip:
                        r = dftu_series_str_strip(A());
                        break;
                    case StrMapOp::Lstrip:
                        r = dftu_series_str_lstrip(A());
                        break;
                    case StrMapOp::Rstrip:
                        r = dftu_series_str_rstrip(A());
                        break;
                }
                s[k] = Series{r};
                break;
            }
            case OP_STR_LEN:
                s[k] = Series{op.param ? dftu_series_str_len_chars(A())
                                       : dftu_series_str_len_bytes(A())};
                break;
            case OP_STR_FIND:
                s[k] = Series{dftu_series_str_find(
                    A(), op.text.data(),
                    static_cast<std::int32_t>(op.text.size()))};
                break;
            case OP_STR_REPLACE: {
                const char* f = op.text.data();
                const auto fn = static_cast<std::int32_t>(op.text.size());
                const char* t = op.text2.data();
                const auto tn = static_cast<std::int32_t>(op.text2.size());
                s[k] = Series{
                    op.param ? dftu_series_str_replace_all(A(), f, fn, t, tn)
                             : dftu_series_str_replace(A(), f, fn, t, tn)};
                break;
            }
            case OP_STR_SLICE:
                s[k] = Series{dftu_series_str_slice(A(), op.scalar.value.i,
                                                    op.scalar2.value.i)};
                break;
            case OP_IS_IN:
                s[k] = Series{dftu_series_is_in(A(), op.values)};
                break;
            case OP_IS_NULL:
                s[k] = Series{op.param ? dftu_series_null_mask(A())
                                       : dftu_series_valid_mask(A())};
                break;
            case OP_CONST:
                s[k] =
                    const_column(static_cast<TypeId>(op.param), op.scalar, len);
                break;
            case OP_SELECT:
                s[k] = Series{dftu_series_where(
                    s[static_cast<std::size_t>(op.c)].handle(), A(), B())};
                break;
            default:
                return {};
        }
    }
    std::vector<Series> outs;
    outs.reserve(finals.size());
    for (int f : finals) outs.push_back(s[static_cast<std::size_t>(f)].share());
    return outs;
}

Series ensure_flat(const Series& c) {
    if (c.encoding() == Encoding::Flat && c.null_count() == 0) return c.share();
    return Series{dftu_series_materialize(c.handle())};
}

constexpr std::int64_t GRAIN = 1 << 16;

}  // namespace

std::vector<Series> eval_many(const std::vector<Expr>& roots,
                              const std::vector<const Series*>& inputs) {
    if (roots.empty())
        throw std::invalid_argument("expr: needs at least one expression");
    if (inputs.empty())
        throw std::invalid_argument("expr: needs at least one input column");

    // Bare column references share the input; the evaluator would slice and
    // concat a copy.
    bool all_cols = true;
    for (const Expr& root : roots) {
        const std::int32_t i = root.valid() ? expr_col_index(root) : -1;
        all_cols &= i >= 0 && static_cast<std::size_t>(i) < inputs.size();
    }
    if (all_cols) {
        std::vector<Series> outs;
        outs.reserve(roots.size());
        for (const Expr& root : roots) {
            const Series& in =
                *inputs[static_cast<std::size_t>(expr_col_index(root))];
            outs.push_back(in.encoding() == Encoding::Flat ? in.share()
                                                           : in.materialize());
        }
        return outs;
    }

    // One compiler for all roots: hash-consing (CSE) spans the whole program.
    std::vector<DataType> input_types;
    input_types.reserve(inputs.size());
    for (const Series* s : inputs) input_types.push_back(s->data_type());
    Compiler c(input_types);
    std::vector<int> finals;
    finals.reserve(roots.size());
    for (const Expr& root : roots) {
        if (!root.valid()) throw std::invalid_argument("expr: null expression");
        Val out = c.compile(root.node().get());
        if (out.is_scalar)
            throw std::invalid_argument(
                "expr: a constant expression has no column");
        finals.push_back(out.slot);
    }

    // Pruner: materialize only the inputs the program actually loads.
    std::vector<bool> used(inputs.size(), false);
    for (const SlotOp& op : c.program)
        if (op.opcode == OP_LOAD)
            used[static_cast<std::size_t>(op.param)] = true;
    std::vector<Series> flat(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i)
        if (used[i]) flat[i] = ensure_flat(*inputs[i]);

    const std::int64_t n = inputs.front()->length();
    if (n <= GRAIN) return eval_chunk(c.program, flat, finals, 0, n);

    const std::int64_t chunks = (n + GRAIN - 1) / GRAIN;
    std::vector<std::vector<Series>> parts(static_cast<std::size_t>(chunks));
    parallel_for(n, GRAIN, [&](std::int64_t b, std::int64_t e) {
        parts[static_cast<std::size_t>(b / GRAIN)] =
            eval_chunk(c.program, flat, finals, b, e - b);
    });
    std::vector<Series> outs;
    outs.reserve(finals.size());
    for (std::size_t j = 0; j < finals.size(); ++j) {
        std::vector<const Series*> ptrs;
        ptrs.reserve(parts.size());
        for (const std::vector<Series>& part : parts) ptrs.push_back(&part[j]);
        outs.push_back(concat_columns(ptrs));
    }
    return outs;
}

Series eval(const Expr& root, const std::vector<const Series*>& inputs) {
    if (!root.valid()) throw std::invalid_argument("expr: null expression");
    std::vector<Series> outs = eval_many({root}, inputs);
    return std::move(outs.front());
}

DataType infer_type(const Expr& root,
                    const std::vector<DataType>& input_types) {
    if (!root.valid()) throw std::invalid_argument("expr: null expression");
    Compiler c(input_types);
    Val out = c.compile(root.node().get());
    if (out.is_scalar)
        throw std::invalid_argument(
            "expr: a constant expression has no column");
    return out.full;
}

}  // namespace dftracer::utils::dataframe

// ---- C ABI ----------------------------------------------------------------

struct dftu_expr {
    dftracer::utils::dataframe::Expr e;
};

namespace dftracer::utils::dataframe {
const Expr& expr_handle_unwrap(const dftu_expr* h) { return h->e; }
dftu_expr* expr_handle_wrap(Expr e) { return new dftu_expr{std::move(e)}; }
}  // namespace dftracer::utils::dataframe

namespace {
dftu_expr* wrap(dataframe::Expr e) { return new dftu_expr{std::move(e)}; }
const dataframe::Expr& unwrap(const dftu_expr* e) { return e->e; }
}  // namespace

extern "C" {

dftu_expr* dftu_expr_col(int32_t index) {
    return wrap(dataframe::expr_col(index));
}
dftu_expr* dftu_expr_lit_i64(int64_t value) {
    return wrap(dataframe::expr_lit(static_cast<std::int64_t>(value)));
}
dftu_expr* dftu_expr_lit_f64(double value) {
    return wrap(dataframe::expr_lit(value));
}
dftu_expr* dftu_expr_binary(int32_t op, const dftu_expr* a,
                            const dftu_expr* b) {
    return wrap(dataframe::expr_binary(static_cast<dataframe::BinaryOp>(op),
                                       unwrap(a), unwrap(b)));
}
dftu_expr* dftu_expr_prim(int32_t prim, const dftu_expr* a) {
    return wrap(
        dataframe::expr_prim(static_cast<dataframe::PrimOp>(prim), unwrap(a)));
}
dftu_expr* dftu_expr_unary(int32_t op, const dftu_expr* a) {
    return wrap(
        dataframe::expr_unary(static_cast<dataframe::UnaryOp>(op), unwrap(a)));
}
dftu_expr* dftu_expr_clip(const dftu_expr* a, dftu_scalar lo, dftu_scalar hi) {
    return wrap(dataframe::expr_clip(unwrap(a), lo, hi));
}
int32_t dftu_expr_col_index(const dftu_expr* e) {
    return e ? dataframe::expr_col_index(unwrap(e)) : -1;
}
int32_t dftu_expr_as_col_cmp(const dftu_expr* e, int32_t* col, int32_t* cmp,
                             dftu_scalar* rhs) {
    if (!e || !col || !cmp || !rhs) return 0;
    std::int32_t c = -1;
    dataframe::CmpOp op{};
    dataframe::Scalar r;
    if (!dataframe::expr_as_col_cmp(unwrap(e), &c, &op, &r)) return 0;
    *col = c;
    *cmp = static_cast<int32_t>(op);
    *rhs = r;
    return 1;
}
int32_t dftu_expr_as_logical(const dftu_expr* e, int32_t* op, dftu_expr** a,
                             dftu_expr** b) {
    if (!e || !op || !a || !b) return 0;
    dataframe::LogicalOp lop{};
    dataframe::Expr lhs, rhs;
    if (!dataframe::expr_as_logical(unwrap(e), &lop, &lhs, &rhs)) return 0;
    *op = static_cast<int32_t>(lop);
    *a = wrap(std::move(lhs));
    *b = wrap(std::move(rhs));
    return 1;
}
int32_t dftu_expr_as_not(const dftu_expr* e, dftu_expr** a) {
    if (!e || !a) return 0;
    dataframe::Expr inner;
    if (!dataframe::expr_as_not(unwrap(e), &inner)) return 0;
    *a = wrap(std::move(inner));
    return 1;
}
int32_t dftu_expr_as_col_str_pred(const dftu_expr* e, int32_t* col, int32_t* op,
                                  const char** pattern, int32_t* pattern_len) {
    if (!e || !col || !op || !pattern || !pattern_len) return 0;
    std::int32_t c = -1;
    dataframe::StrPredOp sop{};
    std::string_view pat;
    if (!dataframe::expr_as_col_str_pred(unwrap(e), &c, &sop, &pat)) return 0;
    *col = c;
    *op = static_cast<int32_t>(sop);
    *pattern = pat.data();
    *pattern_len = static_cast<int32_t>(pat.size());
    return 1;
}
int32_t dftu_expr_as_col_is_in(const dftu_expr* e, int32_t* col,
                               dftu_series** values) {
    if (!e || !col || !values) return 0;
    std::int32_t c = -1;
    dataframe::Series v;
    if (!dataframe::expr_as_col_is_in(unwrap(e), &c, &v)) return 0;
    *col = c;
    *values = v.release();
    return 1;
}
dftu_expr* dftu_expr_cmp(int32_t cmp, const dftu_expr* a, dftu_scalar rhs) {
    return wrap(dataframe::expr_cmp(static_cast<dataframe::CmpOp>(cmp),
                                    unwrap(a), rhs));
}
dftu_expr* dftu_expr_logical(int32_t op, const dftu_expr* a,
                             const dftu_expr* b) {
    return wrap(dataframe::expr_logical(static_cast<dataframe::LogicalOp>(op),
                                        unwrap(a), unwrap(b)));
}
dftu_expr* dftu_expr_not(const dftu_expr* a) {
    return wrap(dataframe::expr_not(unwrap(a)));
}
dftu_expr* dftu_expr_cast(int32_t type, const dftu_expr* a) {
    return wrap(
        dataframe::expr_cast(static_cast<dataframe::TypeId>(type), unwrap(a)));
}
dftu_expr* dftu_expr_lower(const dftu_expr* a) {
    return wrap(dataframe::expr_lower(unwrap(a)));
}
dftu_expr* dftu_expr_str_pred(int32_t op, const dftu_expr* a,
                              const char* pattern, int32_t pattern_len) {
    if (!a || (pattern_len > 0 && !pattern)) return nullptr;
    return wrap(dataframe::expr_str_pred(
        static_cast<dataframe::StrPredOp>(op), unwrap(a),
        std::string_view(pattern ? pattern : "",
                         static_cast<std::size_t>(pattern_len))));
}
dftu_expr* dftu_expr_str_map(int32_t op, const dftu_expr* a) {
    if (!a) return nullptr;
    return wrap(dataframe::expr_str_map(static_cast<dataframe::StrMapOp>(op),
                                        unwrap(a)));
}
dftu_expr* dftu_expr_str_len(const dftu_expr* a, int32_t chars) {
    if (!a) return nullptr;
    return wrap(dataframe::expr_str_len(unwrap(a), chars != 0));
}
dftu_expr* dftu_expr_str_find(const dftu_expr* a, const char* needle,
                              int32_t needle_len) {
    if (!a || (needle_len > 0 && !needle)) return nullptr;
    return wrap(dataframe::expr_str_find(
        unwrap(a), std::string_view(needle ? needle : "",
                                    static_cast<std::size_t>(needle_len))));
}
dftu_expr* dftu_expr_str_replace(const dftu_expr* a, const char* from,
                                 int32_t from_len, const char* to,
                                 int32_t to_len, int32_t all) {
    if (!a || (from_len > 0 && !from) || (to_len > 0 && !to)) return nullptr;
    return wrap(dataframe::expr_str_replace(
        unwrap(a),
        std::string_view(from ? from : "", static_cast<std::size_t>(from_len)),
        std::string_view(to ? to : "", static_cast<std::size_t>(to_len)),
        all != 0));
}
dftu_expr* dftu_expr_str_slice(const dftu_expr* a, int64_t start, int64_t len) {
    if (!a) return nullptr;
    return wrap(dataframe::expr_str_slice(unwrap(a), start, len));
}
dftu_expr* dftu_expr_is_in(const dftu_expr* a, const dftu_series* values) {
    if (!a || !values) return nullptr;
    return wrap(dataframe::expr_is_in(
        unwrap(a), dataframe::Series{dftu_series_share(values)}));
}
dftu_expr* dftu_expr_select(const dftu_expr* cond, const dftu_expr* a,
                            const dftu_expr* b) {
    if (!cond || !a || !b) return nullptr;
    return wrap(dataframe::expr_select(unwrap(cond), unwrap(a), unwrap(b)));
}
dftu_expr* dftu_expr_is_null(const dftu_expr* a, int32_t null) {
    if (!a) return nullptr;
    return wrap(dataframe::expr_is_null(unwrap(a), null != 0));
}
void dftu_expr_free(dftu_expr* e) { delete e; }

dftu_series* dftu_expr_eval(const dftu_expr* root,
                            const dftu_series* const* inputs,
                            int32_t n_inputs) {
    if (!root) return nullptr;
    std::vector<dataframe::Series> owned;
    std::vector<const dataframe::Series*> cols;
    owned.reserve(static_cast<std::size_t>(n_inputs));
    cols.reserve(static_cast<std::size_t>(n_inputs));
    for (int32_t i = 0; i < n_inputs; ++i) {
        owned.emplace_back(const_cast<dftu_series*>(inputs[i]));
        cols.push_back(&owned.back());
    }
    dftu_series* out = nullptr;
    try {
        out = dataframe::eval(unwrap(root), cols).release();
    } catch (const std::exception&) {
        out = nullptr;
    }
    for (dataframe::Series& c : owned)
        c.release();  // borrowed inputs, do not free
    return out;
}

int32_t dftu_expr_eval_many(const dftu_expr* const* roots, int32_t n_roots,
                            const dftu_series* const* inputs, int32_t n_inputs,
                            dftu_series** out) {
    if (!roots || n_roots <= 0) return -1;
    std::vector<dataframe::Expr> exprs;
    exprs.reserve(static_cast<std::size_t>(n_roots));
    for (int32_t i = 0; i < n_roots; ++i) {
        if (!roots[i]) return -1;
        exprs.push_back(unwrap(roots[i]));
    }
    std::vector<dataframe::Series> owned;
    std::vector<const dataframe::Series*> cols;
    owned.reserve(static_cast<std::size_t>(n_inputs));
    cols.reserve(static_cast<std::size_t>(n_inputs));
    for (int32_t i = 0; i < n_inputs; ++i) {
        owned.emplace_back(const_cast<dftu_series*>(inputs[i]));
        cols.push_back(&owned.back());
    }
    int32_t written = -1;
    try {
        std::vector<dataframe::Series> res = dataframe::eval_many(exprs, cols);
        for (std::size_t i = 0; i < res.size(); ++i) out[i] = res[i].release();
        written = static_cast<int32_t>(res.size());
    } catch (const std::exception&) {
        written = -1;
    }
    for (dataframe::Series& c : owned)
        c.release();  // borrowed inputs, do not free
    return written;
}
}
