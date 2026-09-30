#include <dftracer/utils/core/common/calendar.h>
#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/dataframe/batch_ops.h>          // concat_columns
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/cell_ops.h>  // cell_to_string
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>
#include <dftracer/utils/dataframe/internal/fingerprint.h>
#include <dftracer/utils/dataframe/internal/scalar.h>     // scalar_as
#include <dftracer/utils/dataframe/kernels/string_ops.h>  // str_pattern
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <dftracer/utils/duql/substr_simd.h>
#include <dftracer/utils/json/json_escape.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace dataframe = dftracer::utils::dataframe;
namespace duql = dftracer::utils::duql;

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
    IsNull,
    LitStr,
    LitBool,
    LitNull,
    Arith,
    Neg,
    CmpExpr,
    Coalesce,
    Extreme,
    Concat,
    Round,
    Log,
    Pow,
    StrSubstr,
    StrPattern,
    StrExtract,
    StrRegexReplace,
    Convert,
    ListLen,
    ListGet,
    ListSum,
    ListContains,
    StrFn,
    StrPredCol,
    StrReplaceCol,
    StrSubstrCol,
    RoundCol,
    StrExtractCol,
    DatePart,
    FormatTime
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
    std::string text2;                                     // str_replace `to`
    std::shared_ptr<const duql::CompiledPattern> pattern;  // text: identity
    std::shared_ptr<const duql::Substitution> subst;       // regex replace
    Series values;                                         // is_in set
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
    c->pattern = n.pattern;
    c->subst = n.subst;
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
namespace {
Expr make_time_node(ExprKind k, std::int32_t part, const Expr& a,
                    std::int64_t ns_per_unit, std::string_view fmt) {
    if (ns_per_unit <= 0)
        throw std::invalid_argument("expr: time unit must be positive");
    auto n = std::make_shared<ExprNode>();
    n->kind = k;
    n->i = part;
    n->scalar.kind = DFTU_SCALAR_TAG_I64;
    n->scalar.value.i = ns_per_unit;
    n->text.assign(fmt);
    n->a = a.node();
    return Expr{std::move(n)};
}
}  // namespace
Expr expr_date_part(const Expr& a, std::int32_t part,
                    std::int64_t ns_per_unit) {
    if (!is_date_part_code(part))
        throw std::invalid_argument("expr: unknown date part");
    return make_time_node(ExprKind::DatePart, part, a, ns_per_unit, {});
}
Expr expr_format_time(const Expr& a, std::string_view fmt,
                      std::int64_t ns_per_unit) {
    const std::string_view bad = invalid_time_format(fmt);
    if (!bad.empty())
        throw std::invalid_argument("expr: invalid time format directive '" +
                                    std::string(bad) + "'");
    return make_time_node(ExprKind::FormatTime, 0, a, ns_per_unit, fmt);
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
Expr expr_str_fn(StrFn fn, const Expr& a, const Expr* b, std::string_view text,
                 std::string_view text2, std::int64_t i0, std::int64_t i1) {
    auto n = std::make_shared<ExprNode>();
    n->kind = ExprKind::StrFn;
    n->i = static_cast<std::int32_t>(fn);
    n->scalar.kind = DFTU_SCALAR_TAG_I64;
    n->scalar.value.i = i0;
    n->scalar2.kind = DFTU_SCALAR_TAG_I64;
    n->scalar2.value.i = i1;
    n->text.assign(text);
    n->text2.assign(text2);
    n->a = a.node();
    if (b != nullptr) n->b = b->node();
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

Expr expr_lit_str(std::string_view value) {
    Expr e = make(ExprKind::LitStr, 0, {}, nullptr, nullptr);
    const_cast<ExprNode*>(e.node().get())->text.assign(value);
    return e;
}
Expr expr_lit_bool(bool value) {
    return make(ExprKind::LitBool, value ? 1 : 0, {}, nullptr, nullptr);
}
Expr expr_lit_null(TypeId type) {
    return make(ExprKind::LitNull, static_cast<std::int32_t>(type), {}, nullptr,
                nullptr);
}
Expr expr_arith(ArithOp op, const Expr& a, const Expr& b) {
    return make(ExprKind::Arith, static_cast<std::int32_t>(op), {}, a.node(),
                b.node());
}
Expr expr_neg(const Expr& a) {
    return make(ExprKind::Neg, 0, {}, a.node(), nullptr);
}
Expr expr_cmp_expr(CmpOp cmp, const Expr& a, const Expr& b) {
    return make(ExprKind::CmpExpr, static_cast<std::int32_t>(cmp), {}, a.node(),
                b.node());
}

namespace {
// An n-ary node as a right fold of binary nodes; every n-ary form here is
// associative.
Expr fold_args(ExprKind k, std::int32_t i, const std::vector<Expr>& args,
               const char* who) {
    if (args.empty())
        throw std::invalid_argument(std::string("expr: ") + who +
                                    " needs at least one operand");
    Expr out = args.back();
    for (auto it = args.rbegin() + 1; it != args.rend(); ++it)
        out = make(k, i, {}, it->node(), out.node());
    return out;
}
}  // namespace

Expr expr_coalesce(const std::vector<Expr>& args) {
    return fold_args(ExprKind::Coalesce, 0, args, "coalesce");
}
Expr expr_extreme(const std::vector<Expr>& args, bool least) {
    return fold_args(ExprKind::Extreme, least ? 1 : 0, args,
                     least ? "least" : "greatest");
}
Expr expr_concat(const std::vector<Expr>& args) {
    if (args.size() == 1)
        return make(ExprKind::Concat, 0, {}, args.front().node(),
                    expr_lit_str("").node());
    return fold_args(ExprKind::Concat, 0, args, "concat");
}
Expr expr_round(const Expr& a, std::int64_t digits) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_I64;
    s.value.i = digits;
    return make(ExprKind::Round, 0, s, a.node(), nullptr);
}
Expr expr_log(const Expr& a) {
    return make(ExprKind::Log, 0, {}, a.node(), nullptr);
}
Expr expr_pow(const Expr& a, const Expr& b) {
    return make(ExprKind::Pow, 0, {}, a.node(), b.node());
}
Expr expr_str_pred_col(StrPredOp op, const Expr& a, const Expr& needle) {
    if (op != StrPredOp::Contains && op != StrPredOp::StartsWith &&
        op != StrPredOp::EndsWith)
        throw std::invalid_argument(
            "expr: a column needle takes contains, starts_with or ends_with");
    return make(ExprKind::StrPredCol, static_cast<std::int32_t>(op), {},
                a.node(), needle.node());
}
Expr expr_str_replace_col(const Expr& a, const Expr& from, const Expr& to) {
    Expr e = make(ExprKind::StrReplaceCol, 0, {}, a.node(), from.node());
    const_cast<ExprNode*>(e.node().get())->c = to.node();
    return e;
}
Expr expr_str_substr_col(const Expr& a, const Expr& start, const Expr* len) {
    Expr e = make(ExprKind::StrSubstrCol, 0, {}, a.node(), start.node());
    if (len != nullptr) const_cast<ExprNode*>(e.node().get())->c = len->node();
    return e;
}
Expr expr_round_col(const Expr& a, const Expr& digits) {
    return make(ExprKind::RoundCol, 0, {}, a.node(), digits.node());
}
Expr expr_str_substr(const Expr& a, std::int64_t start, std::int64_t len) {
    auto n = std::make_shared<ExprNode>();
    n->kind = ExprKind::StrSubstr;
    n->scalar.kind = DFTU_SCALAR_TAG_I64;
    n->scalar.value.i = start;
    n->scalar2.kind = DFTU_SCALAR_TAG_I64;
    n->scalar2.value.i = len;
    n->a = a.node();
    return Expr{std::move(n)};
}
namespace {
// A compiled pattern has no source text to persist, so each pattern node
// gets an identity no other node in any process shares: a persisted plan
// that holds one never matches another, where a shared text could match a
// different pattern.
std::string pattern_identity() {
    static const std::string seed = [] {
        std::random_device rd;
        return std::to_string(rd()) + '.' + std::to_string(rd()) + '.' +
               std::to_string(rd());
    }();
    static std::atomic<std::uint64_t> next{0};
    return "pattern:" + seed + ':' + std::to_string(next++);
}

Expr pattern_node(ExprKind k, const Expr& a,
                  std::shared_ptr<const duql::CompiledPattern> p,
                  std::int64_t group) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_I64;
    s.value.i = group;
    Expr e = make(k, 0, s, a.node(), nullptr);
    auto* n = const_cast<ExprNode*>(e.node().get());
    n->pattern = std::move(p);
    n->text = pattern_identity();
    return e;
}
}  // namespace

Expr expr_str_pattern(const Expr& a,
                      std::shared_ptr<const duql::CompiledPattern> p) {
    return pattern_node(ExprKind::StrPattern, a, std::move(p), 0);
}
Expr expr_str_extract(const Expr& a,
                      std::shared_ptr<const duql::CompiledPattern> p,
                      std::int64_t group) {
    return pattern_node(ExprKind::StrExtract, a, std::move(p), group);
}
Expr expr_str_extract_col(const Expr& a,
                          std::shared_ptr<const duql::CompiledPattern> p,
                          const Expr& group) {
    Expr e = pattern_node(ExprKind::StrExtractCol, a, std::move(p), 0);
    const_cast<ExprNode*>(e.node().get())->b = group.node();
    return e;
}
Expr expr_str_regex_replace(const Expr& a,
                            std::shared_ptr<const duql::CompiledPattern> p,
                            duql::Substitution sub) {
    Expr e = pattern_node(ExprKind::StrRegexReplace, a, std::move(p), 0);
    const_cast<ExprNode*>(e.node().get())->subst =
        std::make_shared<const duql::Substitution>(std::move(sub));
    return e;
}
Expr expr_convert(ConvertOp op, const Expr& a) {
    return make(ExprKind::Convert, static_cast<std::int32_t>(op), {}, a.node(),
                nullptr);
}
Expr expr_list_len(const Expr& a) {
    return make(ExprKind::ListLen, 0, {}, a.node(), nullptr);
}
Expr expr_list_get(const Expr& a, std::int64_t index) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_I64;
    s.value.i = index;
    return make(ExprKind::ListGet, 0, s, a.node(), nullptr);
}
Expr expr_list_sum(const Expr& a) {
    return make(ExprKind::ListSum, 0, {}, a.node(), nullptr);
}
Expr expr_list_contains(const Expr& a, Scalar value) {
    Expr e = make(ExprKind::ListContains, 0, value, a.node(), nullptr);
    const dftu_scalar raw = value;
    if (raw.kind == DFTU_SCALAR_TAG_STR) {
        auto* n = const_cast<ExprNode*>(e.node().get());
        n->text.assign(raw.value.s != nullptr ? raw.value.s : "", raw.len);
        n->scalar.value.s = n->text.data();
        n->scalar.len = static_cast<std::uint32_t>(n->text.size());
    }
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
    OP_CONST,    // a column of `param` type filled with `scalar`
    OP_SELECT,   // c ? a : b, with the mask in slot `c`
    OP_IS_NULL,  // the null mask of slot `a` (param 1) or the valid mask (0)
    OP_CONST_NULL,
    OP_TO_I64,   // Uint64 -> Int64, null past int64
    OP_ARITH,
    OP_NEG,
    OP_CMP_EXPR,
    OP_EXTREME,
    OP_CONCAT,
    OP_ROUND,
    OP_LOG,
    OP_POW,
    OP_STR_SUBSTR,
    OP_STR_PATTERN,
    OP_STR_EXTRACT,
    OP_STR_REGEX_REPLACE,
    OP_CONVERT,
    OP_LIST_LEN,
    OP_LIST_GET,
    OP_LIST_SUM,
    OP_LIST_CONTAINS,
    OP_STR_FN,  // param = StrFn; scalar/scalar2 = i0/i1; text/text2
    OP_STR_PRED_COL,
    OP_STR_REPLACE_COL,
    OP_STR_SUBSTR_COL,
    OP_ROUND_COL,
    OP_STR_EXTRACT_COL,
    OP_DATE_PART,   // param = part code, scalar = ns per unit
    OP_FORMAT_TIME  // text = format, scalar = ns per unit
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
    // OP_STR_PATTERN / OP_STR_EXTRACT, keyed by the node identity in `text`.
    const duql::CompiledPattern* pattern = nullptr;
    const duql::Substitution* subst = nullptr;
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

bool is_int_type(TypeId t) {
    switch (t) {
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
            return true;
        default:
            return false;
    }
}

bool is_num_type(TypeId t) {
    return is_int_type(t) || t == TypeId::Float32 || t == TypeId::Float64;
}

bool is_text_type(TypeId t) {
    return t == TypeId::String || t == TypeId::LargeString;
}

// The type two numeric operands meet at: either float makes Float64, two
// different integer types meet at Int64.
TypeId promote(TypeId a, TypeId b) {
    if (a == TypeId::Float64 || b == TypeId::Float64 || a == TypeId::Float32 ||
        b == TypeId::Float32)
        return TypeId::Float64;
    return a == b && (a == TypeId::Int64 || a == TypeId::Uint64)
               ? a
               : TypeId::Int64;
}

TypeId arith_type(std::int32_t op, TypeId a, TypeId b) {
    return static_cast<ArithOp>(op) == ArithOp::Div ? TypeId::Float64
           : promote(a, b) == TypeId::Float64       ? TypeId::Float64
                                                    : TypeId::Int64;
}

// Depends only on each input column's DataType, never its data, so the same
// compile() drives eval() (Series::data_type()) and infer_type() (a schema
// with no data) and the two can never disagree.
class Compiler {
   public:
    explicit Compiler(const std::vector<DataType>& input_types)
        : input_types_(input_types) {}

    // A root that folded to a scalar, as a column of that value in every row.
    Val column(const Val& v) {
        if (!v.is_scalar) return v;
        return broadcast(
            v.scalar, v.scalar.kind == DFTU_SCALAR_TAG_F64   ? TypeId::Float64
                      : v.scalar.kind == DFTU_SCALAR_TAG_U64 ? TypeId::Uint64
                                                             : TypeId::Int64);
    }

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
            case ExprKind::DatePart:
            case ExprKind::FormatTime: {
                const bool fmt = n->kind == ExprKind::FormatTime;
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                if (!a0.is_scalar && !is_num_type(a0.type))
                    throw std::invalid_argument(
                        std::string("expr: ") +
                        (fmt ? "format_time" : "date_part") +
                        " needs a time or integer operand, got " +
                        type_name(a0.type));
                Val a = num(a0, fmt ? "format_time" : "date_part");
                SlotOp op;
                op.opcode = fmt ? OP_FORMAT_TIME : OP_DATE_PART;
                op.a = a.slot;
                op.param = n->i;
                op.scalar = n->scalar;
                op.text = n->text;
                return col_val(emit_op(op),
                               fmt ? TypeId::String : TypeId::Int64);
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
            case ExprKind::LitStr: {
                dftu_scalar s{};
                s.kind = DFTU_SCALAR_TAG_STR;
                s.value.s = n->text.data();
                s.len = static_cast<std::uint32_t>(n->text.size());
                return broadcast(s, TypeId::String);
            }
            case ExprKind::LitBool: {
                dftu_scalar s{};
                s.kind = DFTU_SCALAR_TAG_I64;
                s.value.i = n->i;
                return broadcast(s, TypeId::Bool);
            }
            case ExprKind::LitNull: {
                const auto t = static_cast<TypeId>(n->i);
                if (!is_num_type(t) && t != TypeId::Bool && t != TypeId::String)
                    throw std::invalid_argument(
                        std::string("expr: no null literal of type ") +
                        type_name(t));
                return col_val(emit(OP_CONST_NULL, -1, -1, n->i, {}), t);
            }
            case ExprKind::Arith: {
                Val a0 = compile(n->a.get());
                Val b0 = compile(n->b.get());
                if (is_unknown(a0) || is_unknown(b0)) return unknown_val();
                Val a = num(a0, "arithmetic");
                Val b = num(b0, "arithmetic");
                const TypeId t = arith_type(n->i, a.type, b.type);
                return col_val(emit(OP_ARITH, a.slot, b.slot, n->i, {}), t);
            }
            case ExprKind::Neg: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = num(a0, "negation");
                const TypeId t =
                    a.type == TypeId::Float64 ? TypeId::Float64 : TypeId::Int64;
                return col_val(emit(OP_NEG, a.slot, -1, 0, {}), t);
            }
            case ExprKind::CmpExpr: {
                Val a0 = compile(n->a.get());
                Val b0 = compile(n->b.get());
                if (is_unknown(a0) || is_unknown(b0)) return unknown_val();
                Val a = operand(a0, "compare");
                Val b = operand(b0, "compare");
                same_domain(a, b, "compare");
                return col_val(emit(OP_CMP_EXPR, a.slot, b.slot, n->i, {}),
                               TypeId::Bool);
            }
            case ExprKind::Coalesce: {
                Val a0 = compile(n->a.get());
                Val b0 = compile(n->b.get());
                if (is_unknown(a0) || is_unknown(b0)) return unknown_val();
                Val a = operand(a0, "coalesce");
                Val b = operand(b0, "coalesce");
                const TypeId t = common_type(a, b, "coalesce");
                // Presence is the operand's own: a value that does not fit
                // `t` is a null result, not a reason to move on to `b`.
                const int present = emit(OP_IS_NULL, a.slot, -1, 0, {});
                a = to_type(a, t);
                b = to_type(b, t);
                SlotOp op;
                op.opcode = OP_SELECT;
                op.a = a.slot;
                op.b = b.slot;
                op.c = present;
                return col_val(emit_op(op), t);
            }
            case ExprKind::Extreme: {
                Val a0 = compile(n->a.get());
                Val b0 = compile(n->b.get());
                if (is_unknown(a0) || is_unknown(b0)) return unknown_val();
                Val a = operand(a0, "least/greatest");
                Val b = operand(b0, "least/greatest");
                same_domain(a, b, "least/greatest");
                const TypeId t = common_type(a, b, "least/greatest");
                return col_val(emit(OP_EXTREME, a.slot, b.slot, n->i, {}), t);
            }
            case ExprKind::Concat: {
                Val a0 = compile(n->a.get());
                Val b0 = compile(n->b.get());
                if (is_unknown(a0) || is_unknown(b0)) return unknown_val();
                Val a = as_str(a0, "concat", false);
                Val b = as_str(b0, "concat", false);
                return col_val(emit(OP_CONCAT, a.slot, b.slot, 0, {}),
                               TypeId::String);
            }
            case ExprKind::Round: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                const bool integer = a0.is_scalar
                                         ? a0.scalar.kind != DFTU_SCALAR_TAG_F64
                                         : is_int_type(a0.type);
                if (integer && n->scalar.value.i >= 0) return a0;
                Val a = num(a0, "round");
                const TypeId t = integer ? TypeId::Int64 : TypeId::Float64;
                return col_val(emit(OP_ROUND, a.slot, -1, 0, n->scalar), t);
            }
            case ExprKind::Log: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = num(a0, "log");
                return col_val(emit(OP_LOG, a.slot, -1, 0, {}),
                               TypeId::Float64);
            }
            case ExprKind::Pow: {
                Val a0 = compile(n->a.get());
                Val b0 = compile(n->b.get());
                if (is_unknown(a0) || is_unknown(b0)) return unknown_val();
                Val a = num(a0, "pow");
                Val b = num(b0, "pow");
                return col_val(emit(OP_POW, a.slot, b.slot, 0, {}),
                               TypeId::Float64);
            }
            case ExprKind::StrSubstr: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                Val a = as_str(a0, "substr", false);
                return col_val(
                    emit(OP_STR_SUBSTR, a.slot, -1, 0, n->scalar, n->scalar2),
                    TypeId::String);
            }
            case ExprKind::StrPredCol: {
                Val a0 = compile(n->a.get());
                Val b0 = compile(n->b.get());
                if (is_unknown(a0) || is_unknown(b0)) return unknown_val();
                Val a = as_str(a0, "string predicate", true);
                Val b = as_str(b0, "string predicate", true);
                return col_val(emit(OP_STR_PRED_COL, a.slot, b.slot, n->i, {}),
                               TypeId::Bool);
            }
            case ExprKind::StrReplaceCol: {
                Val a0 = compile(n->a.get());
                Val b0 = compile(n->b.get());
                Val c0 = compile(n->c.get());
                if (is_unknown(a0) || is_unknown(b0) || is_unknown(c0))
                    return unknown_val();
                Val a = as_str(a0, "string replace", false);
                Val b = as_str(b0, "string replace", false);
                Val c = as_str(c0, "string replace", false);
                SlotOp op;
                op.opcode = OP_STR_REPLACE_COL;
                op.a = a.slot;
                op.b = b.slot;
                op.c = c.slot;
                return col_val(emit_op(op), TypeId::String);
            }
            case ExprKind::StrSubstrCol: {
                Val a0 = compile(n->a.get());
                Val b0 = compile(n->b.get());
                Val c0 = n->c ? compile(n->c.get()) : Val{};
                if (is_unknown(a0) || is_unknown(b0) ||
                    (n->c && is_unknown(c0)))
                    return unknown_val();
                Val a = as_str(a0, "substr", false);
                Val b = num(b0, "substr");
                SlotOp op;
                op.opcode = OP_STR_SUBSTR_COL;
                op.a = a.slot;
                op.b = b.slot;
                if (n->c) op.c = num(c0, "substr").slot;
                return col_val(emit_op(op), TypeId::String);
            }
            case ExprKind::RoundCol: {
                Val a0 = compile(n->a.get());
                Val b0 = compile(n->b.get());
                if (is_unknown(a0) || is_unknown(b0)) return unknown_val();
                const bool integer = a0.is_scalar
                                         ? a0.scalar.kind != DFTU_SCALAR_TAG_F64
                                         : is_int_type(a0.type);
                Val a = num(a0, "round");
                Val b = num(b0, "round");
                const bool unsigned_in =
                    a0.is_scalar ? a0.scalar.kind == DFTU_SCALAR_TAG_U64
                                 : a0.type == TypeId::Uint64;
                return col_val(emit(OP_ROUND_COL, a.slot, b.slot, 0, {}),
                               unsigned_in ? TypeId::Uint64
                               : integer   ? TypeId::Int64
                                           : TypeId::Float64);
            }
            case ExprKind::StrPattern:
            case ExprKind::StrExtract:
            case ExprKind::StrExtractCol:
            case ExprKind::StrRegexReplace:
                return compile_pattern(n);
            case ExprKind::Convert:
                return compile_convert(n);
            case ExprKind::ListLen: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                as_list(a0, "list_len");
                return col_val(emit(OP_LIST_LEN, a0.slot, -1, 0, {}),
                               TypeId::Int64);
            }
            case ExprKind::ListGet: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                const DataType elem = as_list(a0, "list_get");
                return {false,
                        emit(OP_LIST_GET, a0.slot, -1, 0, n->scalar),
                        elem.id,
                        {},
                        elem};
            }
            case ExprKind::ListSum: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                const TypeId e = as_list(a0, "list_sum").id;
                const TypeId t = e == TypeId::Float32 || e == TypeId::Float64
                                     ? TypeId::Float64
                                     : TypeId::Int64;
                return col_val(emit(OP_LIST_SUM, a0.slot, -1, 0, {}), t);
            }
            case ExprKind::ListContains: {
                Val a0 = compile(n->a.get());
                if (is_unknown(a0)) return unknown_val();
                as_list(a0, "list_contains");
                return col_val(
                    emit(OP_LIST_CONTAINS, a0.slot, -1, 0, n->scalar),
                    TypeId::Bool);
            }
            case ExprKind::StrFn:
                return compile_str_fn(n);
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
    static Val col_val(int slot, TypeId t) {
        return {false, slot, t, {}, scalar(t)};
    }

    // A numeric operand as Int64, Uint64 or Float64, the three types the
    // duql kernels read; a literal broadcasts.
    Val num(Val v, const char* who) {
        if (v.is_scalar)
            return broadcast(v.scalar, v.scalar.kind == DFTU_SCALAR_TAG_F64
                                           ? TypeId::Float64
                                       : v.scalar.kind == DFTU_SCALAR_TAG_U64
                                           ? TypeId::Uint64
                                           : TypeId::Int64);
        if (!is_num_type(v.type))
            throw std::invalid_argument(std::string("expr: ") + who +
                                        " needs a numeric operand, got " +
                                        type_name(v.type));
        if (v.type == TypeId::Int64 || v.type == TypeId::Uint64 ||
            v.type == TypeId::Float64)
            return v;
        return cast(v, is_int_type(v.type) ? TypeId::Int64 : TypeId::Float64);
    }

    // An operand of a comparison-like node: numeric ones as num() makes
    // them, the rest as they are.
    Val operand(Val v, const char* who) {
        if (v.is_scalar || is_num_type(v.type)) return num(v, who);
        return v;
    }

    static void same_domain(const Val& a, const Val& b, const char* who) {
        const bool ok = (is_num_type(a.type) && is_num_type(b.type)) ||
                        (is_text_type(a.type) && is_text_type(b.type)) ||
                        (a.type == TypeId::Bool && b.type == TypeId::Bool);
        if (!ok)
            throw std::invalid_argument(std::string("expr: ") + who + " of " +
                                        type_name(a.type) + " and " +
                                        type_name(b.type) + " has no ordering");
    }

    static TypeId common_type(const Val& a, const Val& b, const char* who) {
        if (is_num_type(a.type) && is_num_type(b.type))
            return promote(a.type, b.type);
        if (a.type != b.type)
            throw std::invalid_argument(
                std::string("expr: ") + who + " needs one type, got " +
                type_name(a.type) + " and " + type_name(b.type));
        return a.type;
    }

    Val to_type(Val v, TypeId t) {
        if (v.type == t) return v;
        if (v.type == TypeId::Uint64 && t == TypeId::Int64)
            return col_val(emit(OP_TO_I64, v.slot, -1, 0, {}), t);
        return cast(v, t);
    }

    // The element type of a List operand, which must be known.
    DataType as_list(const Val& v, const char* who) {
        if (v.is_scalar || v.type != TypeId::List || v.full.fields.size() != 1)
            throw std::invalid_argument(
                std::string("expr: ") + who +
                " needs a List column with a known element type, got " +
                type_name(v.is_scalar ? TypeId::Int64 : v.type));
        return v.full.fields.front().type;
    }

    Val compile_pattern(const ExprNode* n) {
        Val a0 = compile(n->a.get());
        if (is_unknown(a0)) return unknown_val();
        const bool extract_col = n->kind == ExprKind::StrExtractCol;
        const bool extract = n->kind == ExprKind::StrExtract || extract_col;
        const bool replace = n->kind == ExprKind::StrRegexReplace;
        const bool text_out = extract || replace;
        Val a = as_str(a0,
                       extract   ? "string extract"
                       : replace ? "string regex replace"
                                 : "string pattern",
                       !text_out);
        if (replace && !n->subst)
            throw std::invalid_argument(
                "expr: a regex replace node needs a substitution");
        if (!n->pattern)
            throw std::invalid_argument("expr: a pattern node needs a pattern");
        if (extract_col) {
            Val g0 = compile(n->b.get());
            if (is_unknown(g0)) return unknown_val();
            SlotOp op;
            op.opcode = OP_STR_EXTRACT_COL;
            op.a = a.slot;
            op.b = num(g0, "string extract").slot;
            op.text = n->text;
            op.pattern = n->pattern.get();
            return col_val(emit_op(op), TypeId::String);
        }
        const std::int64_t group = n->scalar.value.i;
        if (extract && (group < 0 || static_cast<std::uint64_t>(group) >
                                         duql::capture_count(*n->pattern)))
            throw std::invalid_argument("expr: extract group " +
                                        std::to_string(group) +
                                        " is not in the pattern");
        SlotOp op;
        op.opcode = extract   ? OP_STR_EXTRACT
                    : replace ? OP_STR_REGEX_REPLACE
                              : OP_STR_PATTERN;
        op.subst = n->subst.get();
        op.a = a.slot;
        op.scalar = n->scalar;
        op.text = n->text;
        op.pattern = n->pattern.get();
        return col_val(emit_op(op), text_out ? TypeId::String : TypeId::Bool);
    }

    Val compile_convert(const ExprNode* n) {
        Val a0 = compile(n->a.get());
        if (is_unknown(a0)) return unknown_val();
        Val a = operand(a0, "convert");
        if (!is_num_type(a.type) && !is_text_type(a.type) &&
            a.type != TypeId::Bool)
            throw std::invalid_argument(
                std::string("expr: convert needs a scalar operand, got ") +
                type_name(a.type));
        const auto op = static_cast<ConvertOp>(n->i);
        if (op == ConvertOp::Int && a.type == TypeId::Int64) return a;
        if (op == ConvertOp::String && a.type == TypeId::String) return a;
        const TypeId t = op == ConvertOp::Int     ? TypeId::Int64
                         : op == ConvertOp::Float ? TypeId::Float64
                                                  : TypeId::String;
        return col_val(emit(OP_CONVERT, a.slot, -1, n->i, {}), t);
    }

    int emit_str_fn(const ExprNode* n, int a, int b) {
        SlotOp op;
        op.opcode = OP_STR_FN;
        op.a = a;
        op.b = b;
        op.param = n->i;
        op.scalar = n->scalar;
        op.scalar2 = n->scalar2;
        op.text = n->text;
        op.text2 = n->text2;
        return emit_op(op);
    }

    // The expression forms of the eager Series.str methods; the types are
    // the eager results'.
    Val compile_str_fn(const ExprNode* n) {
        const auto fn = static_cast<StrFn>(n->i);
        Val a0 = compile(n->a.get());
        Val b0{};
        if (fn == StrFn::Cat) {
            if (!n->b)
                throw std::invalid_argument(
                    "expr: cat needs another column; the one-column join is "
                    "an aggregate (use Series.str.cat)");
            b0 = compile(n->b.get());
            if (is_unknown(b0)) return unknown_val();
        }
        if (is_unknown(a0)) return unknown_val();
        const DataType list_str = list_of(scalar(TypeId::String));
        if (fn == StrFn::Join) {
            const DataType elem = as_list(a0, "join");
            if (elem.id != TypeId::String)
                throw std::invalid_argument(
                    std::string("expr: join needs a List<String> column, got "
                                "List<") +
                    type_name(elem.id) + ">");
            return col_val(emit_str_fn(n, a0.slot, -1), TypeId::String);
        }
        if (fn == StrFn::Get && !a0.is_scalar && a0.type == TypeId::List) {
            const DataType elem = as_list(a0, "get");
            return {false,
                    emit(OP_LIST_GET, a0.slot, -1, 0, n->scalar),
                    elem.id,
                    {},
                    elem};
        }
        Val a = as_str(a0, "string method", false);
        switch (fn) {
            case StrFn::IsAlnum:
            case StrFn::IsAlpha:
            case StrFn::IsDigit:
            case StrFn::IsDecimal:
            case StrFn::IsNumeric:
            case StrFn::IsSpace:
            case StrFn::IsLower:
            case StrFn::IsUpper:
            case StrFn::IsTitle:
                return col_val(emit_str_fn(n, a.slot, -1), TypeId::Bool);
            case StrFn::Split:
            case StrFn::Partition:
            case StrFn::RPartition:
            case StrFn::Findall:
                return {false,
                        emit_str_fn(n, a.slot, -1),
                        TypeId::List,
                        {},
                        list_str};
            case StrFn::Rfind:
            case StrFn::Index:
            case StrFn::Rindex:
                return col_val(emit_str_fn(n, a.slot, -1), TypeId::Int64);
            case StrFn::Get:
                if (n->scalar.value.i < 0)
                    throw std::invalid_argument(
                        "expr: get with a negative position needs a List "
                        "column; use slice");
                return col_val(emit_str_fn(n, a.slot, -1), TypeId::String);
            case StrFn::Cat: {
                Val b = as_str(b0, "cat", false);
                return col_val(emit_str_fn(n, a.slot, b.slot), TypeId::String);
            }
            case StrFn::Join:
                break;
            default:
                return col_val(emit_str_fn(n, a.slot, -1), TypeId::String);
        }
        throw std::invalid_argument("expr: unknown string method");
    }

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
        // scalar - col and scalar / col have no scalar-first kernel: broadcast
        // the scalar to a column of the result type and run the column kernel.
        const Val left = broadcast(a.scalar, out_type);
        return {false,
                emit(COL_OP[op], left.slot, b.slot, 0, {}),
                out_type,
                {},
                scalar(out_type)};
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
    if (byte_width(t, in.data_type().fixed_size()))
        return in.slice(offset, len);
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
        case TypeId::String: {
            const std::string_view v(s.value.s ? s.value.s : "", s.len);
            const std::vector<std::string_view> rows(
                static_cast<std::size_t>(len), v);
            return Series::strings(std::span<const std::string_view>(rows));
        }
        case TypeId::Unknown:
        case TypeId::Float16:
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

// ---- duql kernels: flat loops over Int64 / Uint64 / Float64, String and Bool
// columns; every unknown result is a null cell
// ---------------------------------

Series flat_view(const Series& s) {
    if (!s.valid() || s.encoding() == Encoding::Flat) return s.share();
    return s.materialize();
}

bool valid_at(const dftu_series* h, std::int64_t i) {
    return !h->validity || ((h->validity->data()[i >> 3] >> (i & 7)) & 1);
}

bool bit_at(const dftu_series* h, std::int64_t i) {
    return (h->data->data()[i >> 3] >> (i & 7)) & 1;
}

template <class T>
const T* values_of(const dftu_series* h) {
    return h->data ? reinterpret_cast<const T*>(h->data->data()) : nullptr;
}

struct TextCol {
    const char* data = "";
    const std::int32_t* off32 = nullptr;
    const std::int64_t* off64 = nullptr;

    explicit TextCol(const dftu_series* h) {
        if (h->data) data = reinterpret_cast<const char*>(h->data->data());
        if (h->offsets && !h->wide_offsets())
            off32 = reinterpret_cast<const std::int32_t*>(h->offsets->data());
        if (h->offsets && h->wide_offsets())
            off64 = reinterpret_cast<const std::int64_t*>(h->offsets->data());
    }
    bool ok() const { return off32 || off64; }
    std::string_view at(std::int64_t i) const {
        if (off32)
            return {data + off32[i],
                    static_cast<std::size_t>(off32[i + 1] - off32[i])};
        return {data + off64[i],
                static_cast<std::size_t>(off64[i + 1] - off64[i])};
    }
};

std::vector<std::uint8_t> bitmap(std::int64_t n) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>((n + 7) / 8), 0);
}

void set_bit(std::vector<std::uint8_t>& bits, std::int64_t i) {
    bits[static_cast<std::size_t>(i >> 3)] |=
        static_cast<std::uint8_t>(1u << (i & 7));
}

// Fixed-width output cells; a row never set is null.
template <class T>
class Cells {
   public:
    explicit Cells(std::int64_t n)
        : vals_(static_cast<std::size_t>(n)), valid_(bitmap(n)) {}
    void set(std::int64_t i, T v) {
        vals_[static_cast<std::size_t>(i)] = v;
        set_bit(valid_, i);
        ++set_;
    }
    void set(std::int64_t i, std::optional<T> v) {
        if (v) set(i, *v);
    }
    Series finish(TypeId t) {
        const auto n = static_cast<std::int64_t>(vals_.size());
        const std::uint8_t* validity = set_ == n ? nullptr : valid_.data();
        const T* data = vals_.data();
        return Series::from_borrowed(t, data, n, std::move(vals_), validity);
    }

   private:
    std::vector<T> vals_;
    std::vector<std::uint8_t> valid_;
    std::int64_t set_ = 0;
};

class BoolCells {
   public:
    explicit BoolCells(std::int64_t n)
        : n_(n), bits_(bitmap(n)), valid_(bitmap(n)) {}
    void set(std::int64_t i, bool v) {
        if (v) set_bit(bits_, i);
        set_bit(valid_, i);
        ++set_;
    }
    Series finish() {
        return Series::flat(TypeId::Bool, bits_.data(), n_,
                            set_ == n_ ? nullptr : valid_.data());
    }

   private:
    std::int64_t n_;
    std::vector<std::uint8_t> bits_;
    std::vector<std::uint8_t> valid_;
    std::int64_t set_ = 0;
};

// String output rows, appended in row order.
class TextCells {
   public:
    explicit TextCells(std::int64_t n) : n_(n), valid_(bitmap(n)) {
        off_.reserve(static_cast<std::size_t>(n) + 1);
        off_.push_back(0);
    }
    std::string& buf() { return data_; }
    void end_row(bool valid) {
        if (valid) {
            set_bit(valid_, static_cast<std::int64_t>(off_.size()) - 1);
            ++set_;
        }
        off_.push_back(static_cast<std::int32_t>(data_.size()));
    }
    void add(std::string_view s) {
        data_.append(s);
        end_row(true);
    }
    Series finish() {
        return Series{
            dftu_series_new_string(DFTU_TYPE_STRING, off_.data(), data_.data(),
                                   n_, set_ == n_ ? nullptr : valid_.data())};
    }

   private:
    std::int64_t n_;
    std::vector<std::int32_t> off_;
    std::string data_;
    std::vector<std::uint8_t> valid_;
    std::int64_t set_ = 0;
};

// Calls fn with a value of the C++ type behind Int64, Uint64 or Float64.
template <class Fn>
void with_num(TypeId t, Fn&& fn) {
    if (t == TypeId::Int64)
        fn(std::int64_t{});
    else if (t == TypeId::Uint64)
        fn(std::uint64_t{});
    else
        fn(double{});
}

template <class T>
constexpr bool IS_DOUBLE = std::is_same_v<T, double>;

template <class T>
int order(T a, T b) {
    return a < b ? -1 : (b < a ? 1 : 0);
}

// Exact comparison of an integer with a double, as the duql evaluator does
// it: the integer is never rounded to a double.
std::optional<int> cmp_num(std::int64_t a, double d) {
    constexpr double TWO_POW_63 = 9223372036854775808.0;
    if (std::isnan(d)) return std::nullopt;
    if (d >= TWO_POW_63) return -1;
    if (d < -TWO_POW_63) return 1;
    const double f = std::floor(d);
    const auto fi = static_cast<std::int64_t>(f);
    if (a != fi) return order(a, fi);
    return d > f ? -1 : 0;
}

std::optional<int> cmp_num(std::uint64_t a, double d) {
    constexpr double TWO_POW_64 = 18446744073709551616.0;
    if (std::isnan(d)) return std::nullopt;
    if (d < 0) return 1;
    if (d >= TWO_POW_64) return -1;
    const double f = std::floor(d);
    const auto fu = static_cast<std::uint64_t>(f);
    if (a != fu) return order(a, fu);
    return d > f ? -1 : 0;
}

std::optional<int> cmp_num(std::int64_t a, std::uint64_t b) {
    if (a < 0) return -1;
    return order(static_cast<std::uint64_t>(a), b);
}

std::optional<int> flip(std::optional<int> c) {
    if (c) return -*c;
    return c;
}

template <class A, class B>
std::optional<int> cmp_numbers(A a, B b) {
    if constexpr (std::is_same_v<A, B>) {
        if constexpr (IS_DOUBLE<A>)
            if (std::isnan(a) || std::isnan(b)) return std::nullopt;
        return order(a, b);
    } else if constexpr (IS_DOUBLE<A>) {
        return flip(cmp_num(b, a));
    } else if constexpr (IS_DOUBLE<B>) {
        return cmp_num(a, b);
    } else if constexpr (std::is_same_v<A, std::int64_t>) {
        return cmp_num(a, b);
    } else {
        return flip(cmp_num(b, a));
    }
}

bool apply_cmp(CmpOp op, int c) {
    switch (op) {
        case CmpOp::Gt:
            return c > 0;
        case CmpOp::Ge:
            return c >= 0;
        case CmpOp::Lt:
            return c < 0;
        case CmpOp::Le:
            return c <= 0;
        case CmpOp::Eq:
            return c == 0;
        case CmpOp::Ne:
            return c != 0;
    }
    return false;
}

// An integer as sign and magnitude, so int64 and uint64 mix without loss.
struct Int {
    bool neg = false;
    std::uint64_t mag = 0;
};

constexpr std::uint64_t U64_MAX = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t I64_MIN_MAG = std::uint64_t{1} << 63;

Int int_of(std::int64_t v) {
    if (v < 0) return {true, std::uint64_t{0} - static_cast<std::uint64_t>(v)};
    return {false, static_cast<std::uint64_t>(v)};
}
Int int_of(std::uint64_t v) { return {false, v}; }

std::optional<std::int64_t> i64_of(Int x) {
    if (!x.neg) {
        if (x.mag > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()))
            return std::nullopt;
        return static_cast<std::int64_t>(x.mag);
    }
    if (x.mag > I64_MIN_MAG) return std::nullopt;
    return static_cast<std::int64_t>(std::uint64_t{0} - x.mag);
}

std::optional<Int> int_add(Int a, Int b) {
    if (a.neg == b.neg) {
        if (a.mag > U64_MAX - b.mag) return std::nullopt;
        return Int{a.neg, a.mag + b.mag};
    }
    if (a.mag >= b.mag) return Int{a.neg, a.mag - b.mag};
    return Int{b.neg, b.mag - a.mag};
}

std::optional<std::int64_t> int_arith(ArithOp op, Int a, Int b) {
    std::optional<Int> out;
    switch (op) {
        case ArithOp::Add:
            out = int_add(a, b);
            break;
        case ArithOp::Sub:
            out = int_add(a, Int{!b.neg, b.mag});
            break;
        case ArithOp::Mul:
            if (a.mag != 0 && b.mag > U64_MAX / a.mag) return std::nullopt;
            out = Int{a.neg != b.neg, a.mag * b.mag};
            break;
        case ArithOp::FloorDiv: {
            if (b.mag == 0) return std::nullopt;
            Int q{a.neg != b.neg, a.mag / b.mag};
            if (q.neg && a.mag % b.mag != 0) ++q.mag;
            out = q;
            break;
        }
        case ArithOp::Mod: {
            if (b.mag == 0) return std::nullopt;
            std::uint64_t rem = a.mag % b.mag;
            if (rem != 0 && a.neg != b.neg) rem = b.mag - rem;
            out = Int{b.neg, rem};
            break;
        }
        case ArithOp::Div:
            return std::nullopt;
    }
    if (!out) return std::nullopt;
    if (out->mag == 0) return 0;
    return i64_of(*out);
}

std::optional<double> of_double(double d) {
    if (std::isnan(d)) return std::nullopt;
    return d;
}

std::optional<double> dbl_arith(ArithOp op, double a, double b) {
    switch (op) {
        case ArithOp::Add:
            return of_double(a + b);
        case ArithOp::Sub:
            return of_double(a - b);
        case ArithOp::Mul:
            return of_double(a * b);
        case ArithOp::Div:
            if (b == 0) return std::nullopt;
            return of_double(a / b);
        case ArithOp::FloorDiv:
            if (b == 0) return std::nullopt;
            return of_double(std::floor(a / b));
        case ArithOp::Mod: {
            if (b == 0) return std::nullopt;
            double rem = std::fmod(a, b);
            if (rem != 0 && (rem < 0) != (b < 0)) rem += b;
            return of_double(rem);
        }
    }
    return std::nullopt;
}

// A number as the output type R: past int64 is null, NaN is null.
template <class R, class V>
std::optional<R> num_as(V v) {
    if constexpr (IS_DOUBLE<R>) {
        return of_double(static_cast<double>(v));
    } else if constexpr (std::is_same_v<R, V>) {
        return v;
    } else if constexpr (std::is_same_v<R, std::int64_t> &&
                         std::is_same_v<V, std::uint64_t>) {
        return i64_of(Int{false, v});
    } else {
        return std::nullopt;
    }
}

// A double truncated to int64; null when it is not finite or out of range.
std::optional<std::int64_t> trunc_i64(double d) {
    if (!std::isfinite(d)) return std::nullopt;
    const double t = std::trunc(d);
    if (t >= -9223372036854775808.0 && t < 9223372036854775808.0)
        return static_cast<std::int64_t>(t);
    return std::nullopt;
}

bool same_rows(const dftu_series* a, const dftu_series* b) {
    return a && b && a->length == b->length;
}

Series arith_kernel(const Series& sa, const Series& sb, std::int32_t code) {
    const Series fa = flat_view(sa);
    const Series fb = flat_view(sb);
    const dftu_series* a = fa.handle();
    const dftu_series* b = fb.handle();
    if (!same_rows(a, b)) return {};
    const auto op = static_cast<ArithOp>(code);
    const std::int64_t n = a->length;
    const TypeId t = arith_type(code, a->type, b->type);
    Series out;
    with_num(a->type, [&](auto xa) {
        with_num(b->type, [&](auto xb) {
            using A = decltype(xa);
            using B = decltype(xb);
            const A* pa = values_of<A>(a);
            const B* pb = values_of<B>(b);
            if constexpr (!IS_DOUBLE<A>) {
                if constexpr (!IS_DOUBLE<B>) {
                    if (t == TypeId::Int64) {
                        Cells<std::int64_t> c(n);
                        for (std::int64_t i = 0; i < n; ++i)
                            if (valid_at(a, i) && valid_at(b, i))
                                c.set(i, int_arith(op, int_of(pa[i]),
                                                   int_of(pb[i])));
                        out = c.finish(TypeId::Int64);
                        return;
                    }
                }
            }
            Cells<double> c(n);
            for (std::int64_t i = 0; i < n; ++i)
                if (valid_at(a, i) && valid_at(b, i))
                    c.set(i, dbl_arith(op, static_cast<double>(pa[i]),
                                       static_cast<double>(pb[i])));
            out = c.finish(TypeId::Float64);
        });
    });
    return out;
}

Series neg_kernel(const Series& sa) {
    const Series fa = flat_view(sa);
    const dftu_series* a = fa.handle();
    if (!a) return {};
    const std::int64_t n = a->length;
    Series out;
    with_num(a->type, [&](auto xa) {
        using A = decltype(xa);
        const A* pa = values_of<A>(a);
        if constexpr (IS_DOUBLE<A>) {
            Cells<double> c(n);
            for (std::int64_t i = 0; i < n; ++i)
                if (valid_at(a, i)) c.set(i, of_double(-pa[i]));
            out = c.finish(TypeId::Float64);
        } else {
            Cells<std::int64_t> c(n);
            for (std::int64_t i = 0; i < n; ++i) {
                if (!valid_at(a, i)) continue;
                const Int x = int_of(pa[i]);
                c.set(i, x.mag == 0 ? std::optional<std::int64_t>{0}
                                    : i64_of(Int{!x.neg, x.mag}));
            }
            out = c.finish(TypeId::Int64);
        }
    });
    return out;
}

bool is_num_col(const dftu_series* h) {
    return h->type == TypeId::Int64 || h->type == TypeId::Uint64 ||
           h->type == TypeId::Float64;
}

// -1/0/1 of row i of `a` against row i of `b` (both present), per domain:
// numbers exactly, strings bytewise, false before true; fn gets the order or
// nullopt when the two do not compare (a NaN).
template <class Fn>
bool order_rows(const dftu_series* a, const dftu_series* b, Fn&& fn) {
    const std::int64_t n = a->length;
    if (is_num_col(a) && is_num_col(b)) {
        with_num(a->type, [&](auto xa) {
            with_num(b->type, [&](auto xb) {
                using A = decltype(xa);
                using B = decltype(xb);
                const A* pa = values_of<A>(a);
                const B* pb = values_of<B>(b);
                for (std::int64_t i = 0; i < n; ++i)
                    if (valid_at(a, i) && valid_at(b, i))
                        fn(i, cmp_numbers(pa[i], pb[i]));
            });
        });
        return true;
    }
    if (a->type == TypeId::Bool && b->type == TypeId::Bool) {
        for (std::int64_t i = 0; i < n; ++i)
            if (valid_at(a, i) && valid_at(b, i))
                fn(i, std::optional<int>(order(bit_at(a, i), bit_at(b, i))));
        return true;
    }
    const TextCol ta(a), tb(b);
    if (!is_text_type(a->type) || !is_text_type(b->type) || !ta.ok() ||
        !tb.ok())
        return false;
    for (std::int64_t i = 0; i < n; ++i)
        if (valid_at(a, i) && valid_at(b, i))
            fn(i, std::optional<int>(order(ta.at(i), tb.at(i))));
    return true;
}

Series cmp_expr_kernel(const Series& sa, const Series& sb, std::int32_t code) {
    const Series fa = flat_view(sa);
    const Series fb = flat_view(sb);
    const dftu_series* a = fa.handle();
    const dftu_series* b = fb.handle();
    if (!same_rows(a, b)) return {};
    const auto op = static_cast<CmpOp>(code);
    BoolCells c(a->length);
    if (!order_rows(a, b, [&](std::int64_t i, std::optional<int> o) {
            if (o) c.set(i, apply_cmp(op, *o));
        }))
        return {};
    return c.finish();
}

Series extreme_kernel(const Series& sa, const Series& sb, bool least) {
    const Series fa = flat_view(sa);
    const Series fb = flat_view(sb);
    const dftu_series* a = fa.handle();
    const dftu_series* b = fb.handle();
    if (!same_rows(a, b)) return {};
    const std::int64_t n = a->length;
    const int want = least ? -1 : 1;
    // Row i takes b only when b is strictly beyond a, as duql keeps the
    // first of equal operands.
    std::vector<char> take_b(static_cast<std::size_t>(n), 0);
    std::vector<char> known(static_cast<std::size_t>(n), 0);
    if (!order_rows(b, a, [&](std::int64_t i, std::optional<int> o) {
            if (!o) return;
            known[static_cast<std::size_t>(i)] = 1;
            take_b[static_cast<std::size_t>(i)] = *o == want;
        }))
        return {};
    auto pick = [&](std::int64_t i) {
        return take_b[static_cast<std::size_t>(i)] != 0;
    };
    auto have = [&](std::int64_t i) {
        return known[static_cast<std::size_t>(i)] != 0;
    };
    if (is_num_col(a)) {
        Series out;
        const TypeId t = promote(a->type, b->type);
        with_num(t, [&](auto xr) {
            with_num(a->type, [&](auto xa) {
                with_num(b->type, [&](auto xb) {
                    using R = decltype(xr);
                    using A = decltype(xa);
                    using B = decltype(xb);
                    const A* pa = values_of<A>(a);
                    const B* pb = values_of<B>(b);
                    Cells<R> c(n);
                    for (std::int64_t i = 0; i < n; ++i)
                        if (have(i))
                            c.set(i, pick(i) ? num_as<R>(pb[i])
                                             : num_as<R>(pa[i]));
                    out = c.finish(t);
                });
            });
        });
        return out;
    }
    if (a->type == TypeId::Bool) {
        BoolCells c(n);
        for (std::int64_t i = 0; i < n; ++i)
            if (have(i)) c.set(i, pick(i) ? bit_at(b, i) : bit_at(a, i));
        return c.finish();
    }
    const TextCol ta(a), tb(b);
    TextCells c(n);
    for (std::int64_t i = 0; i < n; ++i) {
        if (have(i))
            c.add(pick(i) ? tb.at(i) : ta.at(i));
        else
            c.end_row(false);
    }
    return c.finish();
}

Series to_i64_kernel(const Series& sa) {
    const Series fa = flat_view(sa);
    const dftu_series* a = fa.handle();
    if (!a || a->type != TypeId::Uint64) return {};
    const auto* p = values_of<std::uint64_t>(a);
    Cells<std::int64_t> c(a->length);
    for (std::int64_t i = 0; i < a->length; ++i)
        if (valid_at(a, i)) c.set(i, i64_of(Int{false, p[i]}));
    return c.finish(TypeId::Int64);
}

// Round half away from zero to `digits` decimals, as the duql evaluator
// does; an integer operand comes back as Int64.
Series round_kernel(const Series& sa, std::int64_t digits) {
    const Series fa = flat_view(sa);
    const dftu_series* a = fa.handle();
    if (!a) return {};
    const std::int64_t n = a->length;
    const bool integer = a->type != TypeId::Float64;
    const bool in_range = digits >= -308 && digits <= 308;
    const double scale = std::pow(10.0, static_cast<double>(digits));
    Series out;
    with_num(a->type, [&](auto xa) {
        using A = decltype(xa);
        const A* pa = values_of<A>(a);
        auto rounded = [&](std::int64_t i) {
            return std::round(static_cast<double>(pa[i]) * scale) / scale;
        };
        if (integer) {
            Cells<std::int64_t> c(n);
            for (std::int64_t i = 0; i < n; ++i)
                if (in_range && valid_at(a, i)) c.set(i, trunc_i64(rounded(i)));
            out = c.finish(TypeId::Int64);
        } else {
            Cells<double> c(n);
            for (std::int64_t i = 0; i < n; ++i)
                if (in_range && valid_at(a, i)) c.set(i, of_double(rounded(i)));
            out = c.finish(TypeId::Float64);
        }
    });
    return out;
}

Series log_kernel(const Series& sa) {
    const Series fa = flat_view(sa);
    const dftu_series* a = fa.handle();
    if (!a) return {};
    Cells<double> c(a->length);
    with_num(a->type, [&](auto xa) {
        using A = decltype(xa);
        const A* pa = values_of<A>(a);
        for (std::int64_t i = 0; i < a->length; ++i) {
            const auto d = static_cast<double>(pa[i]);
            if (valid_at(a, i) && d > 0) c.set(i, std::log(d));
        }
    });
    return c.finish(TypeId::Float64);
}

Series pow_kernel(const Series& sa, const Series& sb) {
    const Series fa = flat_view(sa);
    const Series fb = flat_view(sb);
    const dftu_series* a = fa.handle();
    const dftu_series* b = fb.handle();
    if (!same_rows(a, b)) return {};
    Cells<double> c(a->length);
    with_num(a->type, [&](auto xa) {
        with_num(b->type, [&](auto xb) {
            using A = decltype(xa);
            using B = decltype(xb);
            const A* pa = values_of<A>(a);
            const B* pb = values_of<B>(b);
            for (std::int64_t i = 0; i < a->length; ++i)
                if (valid_at(a, i) && valid_at(b, i))
                    c.set(i, of_double(std::pow(static_cast<double>(pa[i]),
                                                static_cast<double>(pb[i]))));
        });
    });
    return c.finish(TypeId::Float64);
}

bool utf8_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Byte offset of code point `n` in `s`, or s.size() past the end.
std::size_t utf8_offset(std::string_view s, std::uint64_t n) {
    std::size_t i = 0;
    while (i < s.size() && n > 0) {
        ++i;
        while (i < s.size() && utf8_continuation(s[i])) ++i;
        --n;
    }
    return i;
}

Series substr_kernel(const Series& sa, std::int64_t start, std::int64_t len) {
    const Series fa = flat_view(sa);
    const dftu_series* a = fa.handle();
    if (!a || !is_text_type(a->type)) return {};
    const TextCol ta(a);
    if (!ta.ok()) return {};
    TextCells c(a->length);
    for (std::int64_t i = 0; i < a->length; ++i) {
        if (start < 0 || !valid_at(a, i)) {
            c.end_row(false);
            continue;
        }
        const std::string_view s = ta.at(i);
        const std::string_view rest =
            s.substr(utf8_offset(s, static_cast<std::uint64_t>(start)));
        c.add(len < 0
                  ? rest
                  : rest.substr(
                        0, utf8_offset(rest, static_cast<std::uint64_t>(len))));
    }
    return c.finish();
}

// A time operand as whole units: a Float64 is floored, and a NaN, an infinity
// or a value past int64 is null.
template <class T>
std::optional<std::int64_t> time_units(T x) {
    if constexpr (IS_DOUBLE<T>) {
        return trunc_i64(std::floor(x));
    } else if constexpr (std::is_same_v<T, std::uint64_t>) {
        if (x > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()))
            return std::nullopt;
        return static_cast<std::int64_t>(x);
    } else {
        return x;
    }
}

Series date_part_kernel(const Series& sa, std::int32_t part,
                        std::int64_t ns_per_unit) {
    const Series fa = flat_view(sa);
    const dftu_series* a = fa.handle();
    if (!a) return {};
    Cells<std::int64_t> c(a->length);
    with_num(a->type, [&](auto xa) {
        using A = decltype(xa);
        const A* pa = values_of<A>(a);
        const auto p = static_cast<DatePart>(part);
        for (std::int64_t i = 0; i < a->length; ++i) {
            if (!valid_at(a, i)) continue;
            if (const auto t = time_units<A>(pa[i]))
                c.set(i, date_part(civil_time(*t, ns_per_unit), p));
        }
    });
    return c.finish(TypeId::Int64);
}

Series format_time_kernel(const Series& sa, std::string_view fmt,
                          std::int64_t ns_per_unit) {
    const Series fa = flat_view(sa);
    const dftu_series* a = fa.handle();
    if (!a) return {};
    TextCells c(a->length);
    with_num(a->type, [&](auto xa) {
        using A = decltype(xa);
        const A* pa = values_of<A>(a);
        for (std::int64_t i = 0; i < a->length; ++i) {
            const auto t = valid_at(a, i) ? time_units<A>(pa[i]) : std::nullopt;
            if (t) format_time(c.buf(), civil_time(*t, ns_per_unit), fmt);
            c.end_row(t.has_value());
        }
    });
    return c.finish();
}

Series extract_kernel(const Series& sa, const duql::CompiledPattern& p,
                      std::size_t group) {
    const Series fa = flat_view(sa);
    const dftu_series* a = fa.handle();
    if (!a || !is_text_type(a->type)) return {};
    const TextCol ta(a);
    if (!ta.ok()) return {};
    TextCells c(a->length);
    for (std::int64_t i = 0; i < a->length; ++i) {
        std::string_view m;
        if (valid_at(a, i) &&
            duql::extract(p, ta.at(i), group, m) == duql::MatchResult::YES)
            c.add(m);
        else
            c.end_row(false);
    }
    return c.finish();
}

// An integer operand column as the scan evaluator reads one: a Float64 cell
// is not an integer and reads as null.
struct IntCol {
    const dftu_series* h;
    const void* p;
    explicit IntCol(const dftu_series* s)
        : h(s), p(s->data ? s->data->data() : nullptr) {}
    std::optional<Int> at(std::int64_t i) const {
        if (!valid_at(h, i)) return std::nullopt;
        if (h->type == TypeId::Int64)
            return int_of(static_cast<const std::int64_t*>(p)[i]);
        if (h->type == TypeId::Uint64)
            return int_of(static_cast<const std::uint64_t*>(p)[i]);
        return std::nullopt;
    }
};

Series str_pred_col_kernel(const Series& sa, const Series& sb,
                           std::int32_t code) {
    const Series fa = flat_view(sa);
    const Series fb = flat_view(sb);
    const dftu_series* a = fa.handle();
    const dftu_series* b = fb.handle();
    if (!same_rows(a, b)) return {};
    const TextCol ta(a);
    const TextCol tb(b);
    if (!ta.ok() || !tb.ok()) return {};
    const auto op = static_cast<StrPredOp>(code);
    BoolCells c(a->length);
    for (std::int64_t i = 0; i < a->length; ++i) {
        if (!valid_at(a, i) || !valid_at(b, i)) continue;
        const std::string_view s = ta.at(i);
        const std::string_view n = tb.at(i);
        bool hit = false;
        if (op == StrPredOp::StartsWith)
            hit = s.starts_with(n);
        else if (op == StrPredOp::EndsWith)
            hit = s.ends_with(n);
        else
            hit = duql::detail::substr_find(
                      s.data(), static_cast<std::int64_t>(s.size()), n.data(),
                      static_cast<std::int64_t>(n.size())) >= 0;
        c.set(i, hit);
    }
    return c.finish();
}

Series str_replace_col_kernel(const Series& sa, const Series& sb,
                              const Series& sc) {
    const Series fa = flat_view(sa);
    const Series fb = flat_view(sb);
    const Series fc = flat_view(sc);
    const dftu_series* a = fa.handle();
    const dftu_series* b = fb.handle();
    const dftu_series* d = fc.handle();
    if (!same_rows(a, b) || !same_rows(a, d)) return {};
    const TextCol ta(a);
    const TextCol tb(b);
    const TextCol td(d);
    if (!ta.ok() || !tb.ok() || !td.ok()) return {};
    TextCells c(a->length);
    for (std::int64_t i = 0; i < a->length; ++i) {
        if (!valid_at(a, i) || !valid_at(b, i) || !valid_at(d, i)) {
            c.end_row(false);
            continue;
        }
        const std::string_view s = ta.at(i);
        const std::string_view from = tb.at(i);
        const std::string_view to = td.at(i);
        std::string& out = c.buf();
        std::size_t pos = 0;
        if (!from.empty()) {
            for (std::size_t hit = s.find(from); hit != std::string_view::npos;
                 hit = s.find(from, pos)) {
                out.append(s.substr(pos, hit - pos));
                out.append(to);
                pos = hit + from.size();
            }
        }
        out.append(s.substr(pos));
        c.end_row(true);
    }
    return c.finish();
}

Series substr_col_kernel(const Series& sa, const Series& sb, const Series* sc) {
    const Series fa = flat_view(sa);
    const Series fb = flat_view(sb);
    const Series fc = sc ? flat_view(*sc) : Series{};
    const dftu_series* a = fa.handle();
    const dftu_series* b = fb.handle();
    const dftu_series* d = fc.handle();
    if (!same_rows(a, b) || (sc && !same_rows(a, d))) return {};
    const TextCol ta(a);
    if (!ta.ok()) return {};
    const IntCol starts(b);
    const IntCol lens(sc ? d : b);
    TextCells c(a->length);
    for (std::int64_t i = 0; i < a->length; ++i) {
        const auto start = valid_at(a, i) ? starts.at(i) : std::nullopt;
        if (!start || start->neg) {
            c.end_row(false);
            continue;
        }
        const std::string_view s = ta.at(i);
        const std::string_view rest = s.substr(utf8_offset(s, start->mag));
        if (!sc) {
            c.add(rest);
            continue;
        }
        const auto len = lens.at(i);
        if (!len || len->neg)
            c.end_row(false);
        else
            c.add(rest.substr(0, utf8_offset(rest, len->mag)));
    }
    return c.finish();
}

Series round_col_kernel(const Series& sa, const Series& sb) {
    const Series fa = flat_view(sa);
    const Series fb = flat_view(sb);
    const dftu_series* a = fa.handle();
    const dftu_series* b = fb.handle();
    if (!same_rows(a, b)) return {};
    const std::int64_t n = a->length;
    const IntCol digits(b);
    constexpr double TWO_POW_63 = 9223372036854775808.0;
    auto rounded = [](double x, const Int& d) {
        const double scale = std::pow(10.0, d.neg ? -static_cast<double>(d.mag)
                                                  : static_cast<double>(d.mag));
        return std::round(x * scale) / scale;
    };
    Series out;
    with_num(a->type, [&](auto xa) {
        using A = decltype(xa);
        const A* pa = values_of<A>(a);
        if constexpr (IS_DOUBLE<A>) {
            Cells<double> c(n);
            for (std::int64_t i = 0; i < n; ++i) {
                const auto d = valid_at(a, i) ? digits.at(i) : std::nullopt;
                if (d && d->mag <= 308) c.set(i, of_double(rounded(pa[i], *d)));
            }
            out = c.finish(TypeId::Float64);
        } else if constexpr (std::is_same_v<A, std::uint64_t>) {
            Cells<std::uint64_t> c(n);
            for (std::int64_t i = 0; i < n; ++i) {
                const auto d = valid_at(a, i) ? digits.at(i) : std::nullopt;
                if (!d) continue;
                if (!d->neg) {
                    c.set(i, pa[i]);
                } else if (d->mag <= 308) {
                    const double r = rounded(static_cast<double>(pa[i]), *d);
                    if (r >= 0 && r < TWO_POW_63)
                        c.set(i, static_cast<std::uint64_t>(r));
                }
            }
            out = c.finish(TypeId::Uint64);
        } else {
            Cells<std::int64_t> c(n);
            for (std::int64_t i = 0; i < n; ++i) {
                const auto d = valid_at(a, i) ? digits.at(i) : std::nullopt;
                if (!d) continue;
                if (!d->neg)
                    c.set(i, pa[i]);
                else if (d->mag <= 308)
                    c.set(i,
                          trunc_i64(rounded(static_cast<double>(pa[i]), *d)));
            }
            out = c.finish(TypeId::Int64);
        }
    });
    return out;
}

Series extract_col_kernel(const Series& sa, const Series& sb,
                          const duql::CompiledPattern& p) {
    const Series fa = flat_view(sa);
    const Series fb = flat_view(sb);
    const dftu_series* a = fa.handle();
    const dftu_series* b = fb.handle();
    if (!same_rows(a, b)) return {};
    const TextCol ta(a);
    if (!ta.ok()) return {};
    const IntCol groups(b);
    const std::uint64_t max_group = duql::capture_count(p);
    TextCells c(a->length);
    for (std::int64_t i = 0; i < a->length; ++i) {
        const auto g = valid_at(a, i) ? groups.at(i) : std::nullopt;
        std::string_view m;
        if (g && !g->neg && g->mag <= max_group &&
            duql::extract(p, ta.at(i), static_cast<std::size_t>(g->mag), m) ==
                duql::MatchResult::YES)
            c.add(m);
        else
            c.end_row(false);
    }
    return c.finish();
}

template <class T>
void append_number(std::string& out, T v) {
    if constexpr (std::is_floating_point_v<T>) {
        char buf[32];
        if (char* end = to_chars_double(buf, buf + sizeof(buf), v))
            out.append(buf, end);
    } else {
        char buf[24];
        const auto res = std::to_chars(buf, buf + sizeof(buf), v);
        out.append(buf, res.ptr);
    }
}

template <class V>
std::optional<V> parse_number(std::string_view s) {
    V out{};
    std::from_chars_result res{};
    if constexpr (std::is_same_v<V, double>)
        res = from_chars_double(s.data(), s.data() + s.size(), out);
    else
        res = std::from_chars(s.data(), s.data() + s.size(), out);
    if (res.ec != std::errc{} || res.ptr != s.data() + s.size())
        return std::nullopt;
    return out;
}

std::optional<std::int64_t> text_to_int(std::string_view s) {
    if (auto i = parse_number<std::int64_t>(s)) return i;
    if (auto d = parse_number<double>(s)) return trunc_i64(*d);
    return std::nullopt;
}

// int(x) / float(x): numbers convert (a double truncates toward zero), Bool
// is 0/1, a String parses; anything out of range or unparseable is null.
template <class R>
Series to_number_kernel(const dftu_series* a, TypeId t) {
    const std::int64_t n = a->length;
    Cells<R> c(n);
    auto convert = [](auto v) -> std::optional<R> {
        if constexpr (IS_DOUBLE<R>) {
            return of_double(static_cast<double>(v));
        } else if constexpr (IS_DOUBLE<decltype(v)>) {
            return trunc_i64(v);
        } else {
            return num_as<R>(v);
        }
    };
    if (a->type == TypeId::Bool) {
        for (std::int64_t i = 0; i < n; ++i)
            if (valid_at(a, i)) c.set(i, static_cast<R>(bit_at(a, i)));
    } else if (is_text_type(a->type)) {
        const TextCol ta(a);
        for (std::int64_t i = 0; i < n; ++i) {
            if (!valid_at(a, i)) continue;
            if constexpr (IS_DOUBLE<R>) {
                const auto d = parse_number<double>(ta.at(i));
                if (d) c.set(i, of_double(*d));
            } else {
                c.set(i, text_to_int(ta.at(i)));
            }
        }
    } else {
        with_num(a->type, [&](auto xa) {
            using A = decltype(xa);
            const A* pa = values_of<A>(a);
            for (std::int64_t i = 0; i < n; ++i)
                if (valid_at(a, i)) c.set(i, convert(pa[i]));
        });
    }
    return c.finish(t);
}

// string(x) and json(x): numbers in decimal or shortest round-trip form,
// Bool as true/false; json quotes a string and writes a null (or NaN) as
// null, string leaves them null.
Series to_text_kernel(const dftu_series* a, bool json) {
    const std::int64_t n = a->length;
    TextCells c(n);
    auto missing = [&]() {
        if (json)
            c.add("null");
        else
            c.end_row(false);
    };
    if (a->type == TypeId::Bool) {
        for (std::int64_t i = 0; i < n; ++i) {
            if (!valid_at(a, i))
                missing();
            else
                c.add(bit_at(a, i) ? "true" : "false");
        }
    } else if (is_text_type(a->type)) {
        const TextCol ta(a);
        for (std::int64_t i = 0; i < n; ++i) {
            if (!valid_at(a, i)) {
                missing();
            } else if (json) {
                c.buf() += '"';
                json::append_json_escaped(c.buf(), ta.at(i));
                c.buf() += '"';
                c.end_row(true);
            } else {
                c.add(ta.at(i));
            }
        }
    } else {
        with_num(a->type, [&](auto xa) {
            using A = decltype(xa);
            const A* pa = values_of<A>(a);
            for (std::int64_t i = 0; i < n; ++i) {
                bool nan = false;
                if constexpr (IS_DOUBLE<A>) nan = std::isnan(pa[i]);
                if (!valid_at(a, i) || nan) {
                    missing();
                } else {
                    append_number(c.buf(), pa[i]);
                    c.end_row(true);
                }
            }
        });
    }
    return c.finish();
}

Series convert_kernel(const Series& sa, std::int32_t code) {
    const Series fa = flat_view(sa);
    const dftu_series* a = fa.handle();
    if (!a) return {};
    switch (static_cast<ConvertOp>(code)) {
        case ConvertOp::Int:
            return to_number_kernel<std::int64_t>(a, TypeId::Int64);
        case ConvertOp::Float:
            return to_number_kernel<double>(a, TypeId::Float64);
        case ConvertOp::String:
            return to_text_kernel(a, false);
        case ConvertOp::Json:
            return to_text_kernel(a, true);
    }
    return {};
}

// A List column's flat parts: its int32 offsets and its element column as
// Int64 / Uint64 / Float64 when the elements are numeric.
struct ListParts {
    Series list;
    Series elems;
    const std::int32_t* off = nullptr;
};

std::optional<ListParts> list_parts(const Series& s) {
    ListParts p;
    p.list = flat_view(s);
    const dftu_series* h = p.list.handle();
    if (!h || h->type != TypeId::List || !h->offsets || !h->child())
        return std::nullopt;
    p.off = reinterpret_cast<const std::int32_t*>(h->offsets->data());
    Series child = flat_view(Series{dftu_series_share(h->child().get())});
    const TypeId t = child.type();
    if (is_num_type(t) && t != TypeId::Int64 && t != TypeId::Uint64 &&
        t != TypeId::Float64)
        child = Series{dftu_series_cast(
            child.handle(),
            static_cast<dftu_dtype>(is_int_type(t) ? TypeId::Int64
                                                   : TypeId::Float64))};
    if (!child.valid()) return std::nullopt;
    p.elems = std::move(child);
    return p;
}

// Sum of each list's elements: integers exactly (past int64 is null),
// doubles in order; a null or non-numeric element makes the row null.
Series list_sum_kernel(const Series& s) {
    auto parts = list_parts(s);
    if (!parts) return {};
    const dftu_series* h = parts->list.handle();
    const dftu_series* e = parts->elems.handle();
    const std::int32_t* off = parts->off;
    const std::int64_t n = h->length;
    if (!is_num_col(e)) {
        Cells<std::int64_t> c(n);
        for (std::int64_t i = 0; i < n; ++i)
            if (valid_at(h, i) && off[i] == off[i + 1]) c.set(i, 0);
        return c.finish(TypeId::Int64);
    }
    Series out;
    with_num(e->type, [&](auto xe) {
        using E = decltype(xe);
        const E* pe = values_of<E>(e);
        if constexpr (IS_DOUBLE<E>) {
            Cells<double> c(n);
            for (std::int64_t i = 0; i < n; ++i) {
                if (!valid_at(h, i)) continue;
                double total = 0;
                bool ok = true;
                for (std::int32_t k = off[i]; k < off[i + 1] && ok; ++k) {
                    ok = valid_at(e, k);
                    total += pe[k];
                }
                if (ok) c.set(i, of_double(total));
            }
            out = c.finish(TypeId::Float64);
        } else {
            Cells<std::int64_t> c(n);
            for (std::int64_t i = 0; i < n; ++i) {
                if (!valid_at(h, i)) continue;
                std::optional<Int> total = Int{};
                for (std::int32_t k = off[i]; k < off[i + 1] && total; ++k)
                    total = valid_at(e, k) ? int_add(*total, int_of(pe[k]))
                                           : std::nullopt;
                if (total)
                    c.set(i, total->mag == 0 ? std::optional<std::int64_t>{0}
                                             : i64_of(*total));
            }
            out = c.finish(TypeId::Int64);
        }
    });
    return out;
}

// Whether an element of each list equals `v`: numbers exactly, strings
// bytewise; an element of another domain never equals it.
Series list_contains_kernel(const Series& s, const dftu_scalar& v) {
    auto parts = list_parts(s);
    if (!parts) return {};
    const dftu_series* h = parts->list.handle();
    const dftu_series* e = parts->elems.handle();
    const std::int32_t* off = parts->off;
    const std::int64_t n = h->length;
    BoolCells c(n);
    auto each_row = [&](auto&& equal) {
        for (std::int64_t i = 0; i < n; ++i) {
            if (!valid_at(h, i)) continue;
            bool hit = false;
            for (std::int32_t k = off[i]; k < off[i + 1] && !hit; ++k)
                hit = valid_at(e, k) && equal(k);
            c.set(i, hit);
        }
    };
    if (v.kind == DFTU_SCALAR_TAG_STR && is_text_type(e->type)) {
        const TextCol te(e);
        const std::string_view want(v.value.s ? v.value.s : "", v.len);
        each_row([&](std::int32_t k) { return te.at(k) == want; });
    } else if (v.kind != DFTU_SCALAR_TAG_STR && is_num_col(e)) {
        const TypeId vt = v.kind == DFTU_SCALAR_TAG_I64   ? TypeId::Int64
                          : v.kind == DFTU_SCALAR_TAG_U64 ? TypeId::Uint64
                                                          : TypeId::Float64;
        with_num(e->type, [&](auto xe) {
            with_num(vt, [&](auto xv) {
                using E = decltype(xe);
                using V = decltype(xv);
                const E* pe = values_of<E>(e);
                const V want = scalar_as<V>(v);
                each_row([&](std::int32_t k) {
                    const auto o = cmp_numbers(pe[k], want);
                    return o && *o == 0;
                });
            });
        });
    } else {
        each_row([](std::int32_t) { return false; });
    }
    return c.finish();
}

// A failure a worker raises for the calling thread (index / rindex finding no
// match); require_evaluated throws it, since a parallel worker cannot.
std::mutex g_eval_error_mutex;
std::string g_eval_error;

void set_eval_error(std::string message) {
    std::lock_guard<std::mutex> lock(g_eval_error_mutex);
    g_eval_error = std::move(message);
}

Series str_const(std::string_view t, std::int64_t len) {
    dftu_scalar sc{};
    sc.kind = DFTU_SCALAR_TAG_STR;
    sc.value.s = t.data();
    sc.len = static_cast<std::uint32_t>(t.size());
    return const_column(TypeId::String, sc, len);
}

// The OP_STR_FN runtime: the same C ABI kernels the eager Series.str calls, so
// an expression gives the eager result.
Series str_fn_eval(const SlotOp& op, const Series& a, const Series* b,
                   std::int64_t len) {
    const dftu_series* h = a.handle();
    const std::int64_t i0 = op.scalar.value.i;
    const std::int64_t i1 = op.scalar2.value.i;
    const char* t = op.text.data();
    const auto tn = static_cast<std::int32_t>(op.text.size());
    const char fill = op.text.empty() ? ' ' : op.text.front();
    const auto fn = static_cast<StrFn>(op.param);
    switch (fn) {
        case StrFn::IsAlnum:
        case StrFn::IsAlpha:
        case StrFn::IsDigit:
        case StrFn::IsDecimal:
        case StrFn::IsNumeric:
        case StrFn::IsSpace:
        case StrFn::IsLower:
        case StrFn::IsUpper:
        case StrFn::IsTitle:
            return Series{dftu_series_str_is(h, op.param)};
        case StrFn::PadStart:
            return Series{dftu_series_str_pad_start(h, i0, fill)};
        case StrFn::PadEnd:
            return Series{dftu_series_str_pad_end(h, i0, fill)};
        case StrFn::Center:
            return Series{dftu_series_str_center(h, i0, fill)};
        case StrFn::Zfill:
            return Series{dftu_series_str_zfill(h, i0)};
        case StrFn::RemovePrefix:
            return Series{dftu_series_str_remove_prefix(h, t, tn)};
        case StrFn::RemoveSuffix:
            return Series{dftu_series_str_remove_suffix(h, t, tn)};
        case StrFn::Repeat:
            return Series{dftu_series_str_repeat(h, i0)};
        case StrFn::SliceReplace: {
            Series head{dftu_series_str_slice(h, 0, i0)};
            Series tail{
                i1 == std::numeric_limits<std::int64_t>::min()
                    ? dftu_series_str_slice(h, 0, 0)
                    : dftu_series_str_slice(h, i1, std::int64_t{1} << 62)};
            if (!head.handle() || !tail.handle()) return Series{};
            const Series mid = str_const(op.text, len);
            Series hm{dftu_series_str_cat(head.handle(), mid.handle())};
            if (!hm.handle()) return Series{};
            return Series{dftu_series_str_cat(hm.handle(), tail.handle())};
        }
        case StrFn::Split:
            return Series{dftu_series_str_split(h, t, tn)};
        case StrFn::Partition:
            return Series{dftu_series_str_partition(h, t, tn, 0)};
        case StrFn::RPartition:
            return Series{dftu_series_str_partition(h, t, tn, 1)};
        case StrFn::Findall:
            return Series{dftu_series_str_findall(h, t, tn)};
        case StrFn::Extract:
            return Series{dftu_series_str_extract(h, t, tn, i0)};
        case StrFn::RegexReplace:
            return Series{dftu_series_str_regex_replace(
                h, t, tn, op.text2.data(),
                static_cast<std::int32_t>(op.text2.size()))};
        case StrFn::Rfind:
            return Series{dftu_series_str_rfind(h, t, tn)};
        case StrFn::Index:
        case StrFn::Rindex: {
            Series r{fn == StrFn::Index ? dftu_series_str_find(h, t, tn)
                                        : dftu_series_str_rfind(h, t, tn)};
            if (!r.handle()) return Series{};
            dftu_scalar minus_one{};
            minus_one.kind = DFTU_SCALAR_TAG_I64;
            minus_one.value.i = -1;
            Series miss{
                dftu_series_compare(r.handle(), DFTU_CMP_EQ, minus_one)};
            if (miss.handle() && dftu_series_any(miss.handle()) != 0) {
                set_eval_error(
                    std::string(fn == StrFn::Index ? "index" : "rindex") +
                    ": substring '" + std::string(op.text) +
                    "' not found in every row");
                return Series{};
            }
            return r;
        }
        case StrFn::Join:
            return Series{dftu_series_list_join(h, t, tn)};
        case StrFn::Get: {
            Series piece{dftu_series_str_slice(h, i0, 1)};
            Series lens{dftu_series_str_len_bytes(h)};
            if (!piece.handle() || !lens.handle()) return Series{};
            dftu_scalar bound{};
            bound.kind = DFTU_SCALAR_TAG_I64;
            bound.value.i = i0;
            Series cond{dftu_series_compare(lens.handle(), DFTU_CMP_GT, bound)};
            const Series nulls = Series::nulls(TypeId::String, len);
            if (!cond.handle()) return Series{};
            return Series{dftu_series_where(cond.handle(), piece.handle(),
                                            nulls.handle())};
        }
        case StrFn::Cat: {
            if (b == nullptr) return Series{};
            if (op.text.empty())
                return Series{dftu_series_str_cat(h, b->handle())};
            const Series sep = str_const(op.text, len);
            Series ab{dftu_series_str_cat(h, sep.handle())};
            if (!ab.handle()) return Series{};
            return Series{dftu_series_str_cat(ab.handle(), b->handle())};
        }
    }
    return Series{};
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
        auto SA = [&]() -> const Series& {
            return s[static_cast<std::size_t>(op.a)];
        };
        auto SB = [&]() -> const Series& {
            return s[static_cast<std::size_t>(op.b)];
        };
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
                    case StrMapOp::Capitalize:
                        r = dftu_series_str_case(A(), 0);
                        break;
                    case StrMapOp::Title:
                        r = dftu_series_str_case(A(), 1);
                        break;
                    case StrMapOp::Swapcase:
                        r = dftu_series_str_case(A(), 2);
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
            case OP_CONST_NULL:
                s[k] = Series::nulls(static_cast<TypeId>(op.param), len);
                break;
            case OP_TO_I64:
                s[k] = to_i64_kernel(SA());
                break;
            case OP_ARITH:
                s[k] = arith_kernel(SA(), SB(), op.param);
                break;
            case OP_NEG:
                s[k] = neg_kernel(SA());
                break;
            case OP_CMP_EXPR:
                s[k] = cmp_expr_kernel(SA(), SB(), op.param);
                break;
            case OP_EXTREME:
                s[k] = extreme_kernel(SA(), SB(), op.param != 0);
                break;
            case OP_CONCAT:
                s[k] = Series{dftu_series_str_cat(A(), B())};
                break;
            case OP_ROUND:
                s[k] = round_kernel(SA(), op.scalar.value.i);
                break;
            case OP_LOG:
                s[k] = log_kernel(SA());
                break;
            case OP_POW:
                s[k] = pow_kernel(SA(), SB());
                break;
            case OP_STR_SUBSTR:
                s[k] =
                    substr_kernel(SA(), op.scalar.value.i, op.scalar2.value.i);
                break;
            case OP_STR_PRED_COL:
                s[k] = str_pred_col_kernel(SA(), SB(), op.param);
                break;
            case OP_STR_REPLACE_COL:
                s[k] = str_replace_col_kernel(
                    SA(), SB(), s[static_cast<std::size_t>(op.c)]);
                break;
            case OP_STR_SUBSTR_COL:
                s[k] = substr_col_kernel(
                    SA(), SB(),
                    op.c >= 0 ? &s[static_cast<std::size_t>(op.c)] : nullptr);
                break;
            case OP_ROUND_COL:
                s[k] = round_col_kernel(SA(), SB());
                break;
            case OP_STR_EXTRACT_COL:
                s[k] = extract_col_kernel(SA(), SB(), *op.pattern);
                break;
            case OP_DATE_PART:
                s[k] = date_part_kernel(SA(), op.param, op.scalar.value.i);
                break;
            case OP_FORMAT_TIME:
                s[k] = format_time_kernel(SA(), op.text, op.scalar.value.i);
                break;
            case OP_STR_PATTERN:
                s[k] = str_pattern(SA(), *op.pattern);
                break;
            case OP_STR_EXTRACT:
                s[k] =
                    extract_kernel(SA(), *op.pattern,
                                   static_cast<std::size_t>(op.scalar.value.i));
                break;
            case OP_STR_REGEX_REPLACE:
                s[k] = str_regex_replace(SA(), *op.pattern, *op.subst);
                break;
            case OP_CONVERT:
                s[k] = convert_kernel(SA(), op.param);
                break;
            case OP_LIST_LEN:
                s[k] = Series{dftu_series_list_len(A())};
                break;
            case OP_LIST_GET:
                s[k] = Series{dftu_series_list_get(A(), op.scalar.value.i)};
                break;
            case OP_LIST_SUM:
                s[k] = list_sum_kernel(SA());
                break;
            case OP_LIST_CONTAINS:
                s[k] = list_contains_kernel(SA(), op.scalar);
                break;
            case OP_STR_FN:
                s[k] = str_fn_eval(op, SA(), op.b >= 0 ? &SB() : nullptr, len);
                break;
            default:
                return {};
        }
        if (!s[k].handle()) return {};
    }
    std::vector<Series> outs;
    outs.reserve(finals.size());
    for (int f : finals) outs.push_back(s[static_cast<std::size_t>(f)].share());
    return outs;
}

// eval_chunk returns no columns when a kernel does not take its input type;
// raised here, on the calling thread, since the parallel workers cannot throw.
void require_evaluated(const std::vector<Series>& outs) {
    if (outs.empty()) {
        std::string message;
        {
            std::lock_guard<std::mutex> lock(g_eval_error_mutex);
            message.swap(g_eval_error);
        }
        if (!message.empty()) throw std::invalid_argument(message);
        throw std::invalid_argument(
            "expr: an operation does not take its input column type");
    }
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
        finals.push_back(c.column(c.compile(root.node().get())).slot);
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
    if (n <= GRAIN) {
        std::vector<Series> outs = eval_chunk(c.program, flat, finals, 0, n);
        require_evaluated(outs);
        return outs;
    }

    const std::int64_t chunks = (n + GRAIN - 1) / GRAIN;
    std::vector<std::vector<Series>> parts(static_cast<std::size_t>(chunks));
    // One task per chunk: without a parallel backend parallel_for runs the
    // whole range as one call, which must still fill every chunk.
    parallel_for(chunks, 1, [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t k = b; k < e; ++k) {
            const std::int64_t lo = k * GRAIN;
            parts[static_cast<std::size_t>(k)] = eval_chunk(
                c.program, flat, finals, lo, std::min(GRAIN, n - lo));
        }
    });
    for (const std::vector<Series>& part : parts) require_evaluated(part);
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
    return c.column(c.compile(root.node().get())).full;
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
dftu_expr* dftu_expr_str_pred_col(int32_t op, const dftu_expr* a,
                                  const dftu_expr* needle) {
    if (!a || !needle) return nullptr;
    if (op != static_cast<int32_t>(dataframe::StrPredOp::Contains) &&
        op != static_cast<int32_t>(dataframe::StrPredOp::StartsWith) &&
        op != static_cast<int32_t>(dataframe::StrPredOp::EndsWith))
        return nullptr;
    return wrap(dataframe::expr_str_pred_col(
        static_cast<dataframe::StrPredOp>(op), unwrap(a), unwrap(needle)));
}
dftu_expr* dftu_expr_str_replace_col(const dftu_expr* a, const dftu_expr* from,
                                     const dftu_expr* to) {
    if (!a || !from || !to) return nullptr;
    return wrap(
        dataframe::expr_str_replace_col(unwrap(a), unwrap(from), unwrap(to)));
}
dftu_expr* dftu_expr_str_substr_col(const dftu_expr* a, const dftu_expr* start,
                                    const dftu_expr* len) {
    if (!a || !start) return nullptr;
    const dataframe::Expr l = len ? unwrap(len) : dataframe::Expr{};
    return wrap(dataframe::expr_str_substr_col(unwrap(a), unwrap(start),
                                               len ? &l : nullptr));
}
dftu_expr* dftu_expr_round_col(const dftu_expr* a, const dftu_expr* digits) {
    if (!a || !digits) return nullptr;
    return wrap(dataframe::expr_round_col(unwrap(a), unwrap(digits)));
}
dftu_expr* dftu_expr_str_map(int32_t op, const dftu_expr* a) {
    if (!a) return nullptr;
    return wrap(dataframe::expr_str_map(static_cast<dataframe::StrMapOp>(op),
                                        unwrap(a)));
}
dftu_expr* dftu_expr_str_fn(int32_t fn, const dftu_expr* a, const dftu_expr* b,
                            const char* text, int32_t text_len,
                            const char* text2, int32_t text2_len, int64_t i0,
                            int64_t i1) {
    if (!a || fn < 0 || fn > DFTU_STR_FN_REGEX_REPLACE ||
        (text_len > 0 && !text) || (text2_len > 0 && !text2))
        return nullptr;
    const dataframe::Expr other = b ? unwrap(b) : dataframe::Expr{};
    return wrap(dataframe::expr_str_fn(
        static_cast<dataframe::StrFn>(fn), unwrap(a), b ? &other : nullptr,
        std::string_view(text ? text : "", static_cast<std::size_t>(text_len)),
        std::string_view(text2 ? text2 : "",
                         static_cast<std::size_t>(text2_len)),
        i0, i1));
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
dftu_expr* dftu_expr_date_part(const dftu_expr* a, int32_t part,
                               int64_t ns_per_unit) {
    if (!a || !dftracer::utils::is_date_part_code(part) || ns_per_unit <= 0)
        return nullptr;
    return wrap(dataframe::expr_date_part(unwrap(a), part, ns_per_unit));
}
dftu_expr* dftu_expr_format_time(const dftu_expr* a, const char* fmt,
                                 int32_t fmt_len, int64_t ns_per_unit) {
    if (!a || fmt_len < 0 || (fmt_len > 0 && !fmt) || ns_per_unit <= 0)
        return nullptr;
    const std::string_view f(fmt ? fmt : "", static_cast<std::size_t>(fmt_len));
    if (!dftracer::utils::invalid_time_format(f).empty()) return nullptr;
    return wrap(dataframe::expr_format_time(unwrap(a), f, ns_per_unit));
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
dftu_expr* dftu_expr_lit_str(const char* value, int32_t len) {
    if (len < 0 || (len > 0 && !value)) return nullptr;
    return wrap(dataframe::expr_lit_str(
        std::string_view(value ? value : "", static_cast<std::size_t>(len))));
}
dftu_expr* dftu_expr_lit_bool(int32_t value) {
    return wrap(dataframe::expr_lit_bool(value != 0));
}
dftu_expr* dftu_expr_lit_null(int32_t type) {
    return wrap(dataframe::expr_lit_null(static_cast<dataframe::TypeId>(type)));
}
dftu_expr* dftu_expr_arith(int32_t op, const dftu_expr* a, const dftu_expr* b) {
    if (!a || !b) return nullptr;
    return wrap(dataframe::expr_arith(static_cast<dataframe::ArithOp>(op),
                                      unwrap(a), unwrap(b)));
}
dftu_expr* dftu_expr_neg(const dftu_expr* a) {
    if (!a) return nullptr;
    return wrap(dataframe::expr_neg(unwrap(a)));
}
dftu_expr* dftu_expr_cmp_expr(int32_t cmp, const dftu_expr* a,
                              const dftu_expr* b) {
    if (!a || !b) return nullptr;
    return wrap(dataframe::expr_cmp_expr(static_cast<dataframe::CmpOp>(cmp),
                                         unwrap(a), unwrap(b)));
}
namespace {
bool expr_args(const dftu_expr* const* args, int32_t n,
               std::vector<dataframe::Expr>& out) {
    if (!args || n <= 0) return false;
    out.reserve(static_cast<std::size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        if (!args[i]) return false;
        out.push_back(unwrap(args[i]));
    }
    return true;
}
}  // namespace
dftu_expr* dftu_expr_coalesce(const dftu_expr* const* args, int32_t n) {
    std::vector<dataframe::Expr> v;
    if (!expr_args(args, n, v)) return nullptr;
    return wrap(dataframe::expr_coalesce(v));
}
dftu_expr* dftu_expr_extreme(const dftu_expr* const* args, int32_t n,
                             int32_t least) {
    std::vector<dataframe::Expr> v;
    if (!expr_args(args, n, v)) return nullptr;
    return wrap(dataframe::expr_extreme(v, least != 0));
}
dftu_expr* dftu_expr_concat(const dftu_expr* const* args, int32_t n) {
    std::vector<dataframe::Expr> v;
    if (!expr_args(args, n, v)) return nullptr;
    return wrap(dataframe::expr_concat(v));
}
dftu_expr* dftu_expr_round(const dftu_expr* a, int64_t digits) {
    if (!a) return nullptr;
    return wrap(dataframe::expr_round(unwrap(a), digits));
}
dftu_expr* dftu_expr_log(const dftu_expr* a) {
    if (!a) return nullptr;
    return wrap(dataframe::expr_log(unwrap(a)));
}
dftu_expr* dftu_expr_pow(const dftu_expr* a, const dftu_expr* b) {
    if (!a || !b) return nullptr;
    return wrap(dataframe::expr_pow(unwrap(a), unwrap(b)));
}
dftu_expr* dftu_expr_str_substr(const dftu_expr* a, int64_t start,
                                int64_t len) {
    if (!a) return nullptr;
    return wrap(dataframe::expr_str_substr(unwrap(a), start, len));
}
dftu_expr* dftu_expr_convert(int32_t op, const dftu_expr* a) {
    if (!a) return nullptr;
    return wrap(dataframe::expr_convert(static_cast<dataframe::ConvertOp>(op),
                                        unwrap(a)));
}
dftu_expr* dftu_expr_list_len(const dftu_expr* a) {
    if (!a) return nullptr;
    return wrap(dataframe::expr_list_len(unwrap(a)));
}
dftu_expr* dftu_expr_list_get(const dftu_expr* a, int64_t index) {
    if (!a) return nullptr;
    return wrap(dataframe::expr_list_get(unwrap(a), index));
}
dftu_expr* dftu_expr_list_sum(const dftu_expr* a) {
    if (!a) return nullptr;
    return wrap(dataframe::expr_list_sum(unwrap(a)));
}
dftu_expr* dftu_expr_list_contains(const dftu_expr* a, dftu_scalar value) {
    if (!a ||
        (value.kind == DFTU_SCALAR_TAG_STR && value.len > 0 && !value.value.s))
        return nullptr;
    return wrap(dataframe::expr_list_contains(unwrap(a), value));
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
