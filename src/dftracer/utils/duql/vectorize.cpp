#include <dftracer/utils/duql/lookup.h>
#include <dftracer/utils/duql/numbers.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <dftracer/utils/duql/vectorize.h>
#include <dftracer/utils/json/canonical.h>
#include <dftracer/utils/json/record_parser.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../dataframe/batch_ops.h"

namespace dftracer::utils::duql {

namespace {

namespace df = dftracer::utils::dataframe;
using df::CmpOp;
using df::Expr;
using df::LogicalOp;
using df::TypeId;

enum class K : std::uint8_t { INT, UINT, FLOAT, STR, BOOL, LIST, NUL, OTHER };

struct V {
    Expr e;
    K k = K::NUL;
    const TConst* constant = nullptr;
    K elem = K::OTHER;  ///< The element kind of a LIST.
};

bool numeric(K k) { return k == K::INT || k == K::UINT || k == K::FLOAT; }

K kind_of(TypeId t) {
    switch (t) {
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
            return K::INT;
        case TypeId::Uint64:
            return K::UINT;
        case TypeId::Float16:
        case TypeId::Float32:
        case TypeId::Float64:
            return K::FLOAT;
        case TypeId::String:
        case TypeId::LargeString:
            return K::STR;
        case TypeId::Bool:
            return K::BOOL;
        case TypeId::List:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
            return K::LIST;
        default:
            return K::OTHER;
    }
}

TypeId type_of(K k) {
    switch (k) {
        case K::UINT:
            return TypeId::Uint64;
        case K::FLOAT:
            return TypeId::Float64;
        case K::STR:
            return TypeId::String;
        case K::BOOL:
            return TypeId::Bool;
        default:
            return TypeId::Int64;
    }
}

struct Failure {
    VectorizeError error;
};

class Vectorizer {
   public:
    Vectorizer(const std::vector<VectorColumn>& columns, bool args_fallback)
        : columns_(columns), args_fallback_(args_fallback) {}

    V operator()(const Term& t) {
        return std::visit([this](const auto& n) { return this->node(n); },
                          t.node);
    }

   private:
    const std::vector<VectorColumn>& columns_;
    bool args_fallback_;

    [[noreturn]] static void fail(std::string msg, bool rows = false) {
        throw Failure{{std::move(msg), rows}};
    }

    [[noreturn]] static void no_kernel(std::string_view what) {
        fail(std::string(what) + " has no column kernel");
    }

    static V null() { return {}; }
    static V boolean(bool b) { return {df::expr_lit_bool(b), K::BOOL}; }
    static Expr as(const V& v, K k) {
        return v.k == K::NUL ? df::expr_lit_null(type_of(k)) : v.e;
    }
    static Expr as_bool(const V& v) {
        return v.k == K::BOOL ? v.e : df::expr_lit_null(TypeId::Bool);
    }

    std::optional<std::size_t> column(std::string_view name) const {
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].name == name) return i;
        return std::nullopt;
    }

    // The column a path names: the path itself or, for a bare name, the
    // `args.` field; with an index step, the list column before it. An
    // element path names `.` or `.<field>`.
    std::optional<std::size_t> field_column(const TField& f,
                                            std::size_t& consumed) const {
        consumed = f.steps.size();
        const bool element = f.root == FieldRoot::ELEMENT;
        std::string key = element ? "." : "";
        std::optional<std::size_t> best;
        if (element)
            if ((best = column(key))) consumed = 0;
        for (std::size_t i = 0; i < f.steps.size(); ++i) {
            const auto& s = f.steps[i];
            if (s.index) {
                if (key.empty()) return std::nullopt;
                key += '[' + std::to_string(*s.index) + ']';
            } else {
                if (i > 0) key += '.';
                key += s.key;
            }
            auto c = column(key);
            if (!c && !element && args_fallback_) c = column("args." + key);
            if (c) {
                best = c;
                consumed = i + 1;
            }
        }
        return best;
    }

    V node(const TConst& c) const {
        return std::visit(
            [&](const auto& v) -> V {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, TNull>)
                    return null();
                else if constexpr (std::is_same_v<T, bool>)
                    return {df::expr_lit_bool(v), K::BOOL, &c};
                else if constexpr (std::is_same_v<T, std::int64_t>)
                    return {df::expr_lit(v), K::INT, &c};
                else if constexpr (std::is_same_v<T, std::uint64_t>) {
                    if (v > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max()))
                        no_kernel("an integer literal above int64");
                    return {df::expr_lit(static_cast<std::int64_t>(v)), K::INT,
                            &c};
                } else if constexpr (std::is_same_v<T, double>)
                    return {df::expr_lit(v), K::FLOAT, &c};
                else
                    return {df::expr_lit_str(v), K::STR, &c};
            },
            c.value);
    }

    V node(const TField& f) const {
        std::size_t consumed = 0;
        const auto c = field_column(f, consumed);
        if (!c) return null();
        const VectorColumn& col = columns_[*c];
        if (col.json) fail("'" + col.name + "' is a json field", true);
        V v{df::expr_col(static_cast<std::int32_t>(*c)), kind_of(col.type.id)};
        const df::DataType* type = &col.type;
        v.elem = element(*type);
        for (std::size_t i = consumed; i < f.steps.size(); ++i) {
            if (v.k != K::LIST || !f.steps[i].index) return null();
            v.e = df::expr_list_get(v.e, *f.steps[i].index);
            type = &type->fields.front().type;
            v.k = kind_of(type->id);
            v.elem = element(*type);
        }
        if (v.k == K::OTHER) return null();
        return v;
    }

    static K element(const df::DataType& t) {
        if (kind_of(t.id) != K::LIST || t.fields.empty()) return K::OTHER;
        return kind_of(t.fields.front().type.id);
    }

    // Whether `t` has a value: a field with a column, and for a column read
    // from records, one whose cell is not null.
    V present(const Term& t) {
        const auto* f = std::get_if<TField>(&t.node);
        if (!f) return boolean(true);
        std::size_t consumed = 0;
        const auto c = field_column(*f, consumed);
        if (!c || consumed != f->steps.size()) return boolean(false);
        if (!columns_[*c].record) return boolean(true);
        return {df::expr_is_null(df::expr_col(static_cast<std::int32_t>(*c)),
                                 false),
                K::BOOL};
    }

    V node(const TUnary& u) {
        const V v = (*this)(*u.operand);
        if (u.op == TermOp::NOT)
            return v.k == K::BOOL ? V{df::expr_not(v.e), K::BOOL} : null();
        if (!numeric(v.k)) return null();
        return {df::expr_neg(v.e), v.k == K::UINT ? K::INT : v.k};
    }

    static K arith_kind(TermOp op, K a, K b) {
        if (op == TermOp::DIV || a == K::FLOAT || b == K::FLOAT)
            return K::FLOAT;
        return K::INT;
    }

    static df::ArithOp arith_op(TermOp op) {
        switch (op) {
            case TermOp::ADD:
                return df::ArithOp::Add;
            case TermOp::SUB:
                return df::ArithOp::Sub;
            case TermOp::MUL:
                return df::ArithOp::Mul;
            case TermOp::DIV:
                return df::ArithOp::Div;
            case TermOp::IDIV:
                return df::ArithOp::FloorDiv;
            default:
                return df::ArithOp::Mod;
        }
    }

    static std::optional<CmpOp> cmp_op(TermOp op) {
        switch (op) {
            case TermOp::EQ:
                return CmpOp::Eq;
            case TermOp::NE:
                return CmpOp::Ne;
            case TermOp::LT:
                return CmpOp::Lt;
            case TermOp::LE:
                return CmpOp::Le;
            case TermOp::GT:
                return CmpOp::Gt;
            case TermOp::GE:
                return CmpOp::Ge;
            default:
                return std::nullopt;
        }
    }

    static bool comparable(K a, K b) {
        if (numeric(a) && numeric(b)) return true;
        return a == b && (a == K::STR || a == K::BOOL);
    }

    // `a op b` over values of one domain; null across domains.
    static V compare(CmpOp op, const V& a, const V& b) {
        if (!comparable(a.k, b.k)) return null();
        if (b.constant && a.k == b.k && (a.k == K::INT || a.k == K::FLOAT)) {
            const auto& c = b.constant->value;
            const dftu_scalar s =
                a.k == K::FLOAT ? df::detail::expr_scalar_d(std::get<double>(c))
                                : df::detail::expr_scalar_i(
                                      std::holds_alternative<std::int64_t>(c)
                                          ? std::get<std::int64_t>(c)
                                          : static_cast<std::int64_t>(
                                                std::get<std::uint64_t>(c)));
            return {df::expr_cmp(op, a.e, s), K::BOOL};
        }
        return {df::expr_cmp_expr(op, a.e, b.e), K::BOOL};
    }

    static V both(LogicalOp op, const V& a, const V& b) {
        return {df::expr_logical(op, as_bool(a), as_bool(b)), K::BOOL};
    }

    // One kind for the operands of a value-choosing form, or a compile error.
    static K common(const std::vector<V>& vs, std::string_view what) {
        K out = K::NUL;
        for (const V& v : vs) {
            if (v.k == K::NUL) continue;
            if (out == K::NUL || out == v.k) {
                out = v.k;
            } else if (numeric(out) && numeric(v.k)) {
                out = out == K::FLOAT || v.k == K::FLOAT ? K::FLOAT : K::INT;
            } else {
                fail(std::string(what) +
                     " chooses between values of different "
                     "types; convert them with int(), "
                     "float() or string()");
            }
        }
        return out;
    }

    V node(const TBinary& b) {
        const V l = (*this)(*b.left);
        const V r = (*this)(*b.right);
        switch (b.op) {
            case TermOp::AND:
                return both(LogicalOp::And, l, r);
            case TermOp::OR:
                return both(LogicalOp::Or, l, r);
            case TermOp::COALESCE:
                return coalesce({l, r}, "the ?? operator");
            default:
                break;
        }
        if (auto op = cmp_op(b.op)) return compare(*op, l, r);
        if (!numeric(l.k) || !numeric(r.k)) return null();
        return {df::expr_arith(arith_op(b.op), l.e, r.e),
                arith_kind(b.op, l.k, r.k)};
    }

    static V coalesce(std::vector<V> vs, std::string_view what) {
        const K k = common(vs, what);
        std::vector<Expr> args;
        for (const V& v : vs)
            if (v.k != K::NUL) args.push_back(v.e);
        if (args.empty()) return null();
        if (args.size() == 1) return {args.front(), k};
        return {df::expr_coalesce(args), k};
    }

    V node(const TBetween& b) {
        const V v = (*this)(*b.subject);
        const V t = both(LogicalOp::And, compare(CmpOp::Ge, v, (*this)(*b.low)),
                         compare(CmpOp::Le, v, (*this)(*b.high)));
        return b.negated ? V{df::expr_not(t.e), K::BOOL} : t;
    }

    V node(const TIs& is) {
        if (is.missing) {
            const V p = present(*is.subject);
            return is.negated ? p : V{df::expr_not(p.e), K::BOOL};
        }
        const V v = (*this)(*is.subject);
        if (v.k == K::NUL) return boolean(!is.negated);
        return {df::expr_is_null(v.e, !is.negated), K::BOOL};
    }

    V node(const TIn& in) {
        const V v = (*this)(*in.subject);
        std::vector<V> items;
        for (const auto& item : in.list) {
            V x = (*this)(*item);
            if (x.k != K::NUL && comparable(v.k, x.k)) items.push_back(x);
        }
        if (items.empty()) return null();
        // YES if any item equals, NO if any compares, else unknown.
        Expr any_equal;
        Expr any_compared;
        for (const V& x : items) {
            const Expr eq = compare(CmpOp::Eq, v, x).e;
            const Expr hit = df::expr_coalesce({eq, df::expr_lit_bool(false)});
            const Expr known = df::expr_is_null(eq, false);
            any_equal = any_equal.valid()
                            ? df::expr_logical(LogicalOp::Or, any_equal, hit)
                            : hit;
            any_compared =
                any_compared.valid()
                    ? df::expr_logical(LogicalOp::Or, any_compared, known)
                    : known;
        }
        Expr t = df::expr_select(
            any_equal, df::expr_lit_bool(true),
            df::expr_select(any_compared, df::expr_lit_bool(false),
                            df::expr_lit_null(TypeId::Bool)));
        return {in.negated ? df::expr_not(t) : t, K::BOOL};
    }

    V node(const TIndex& x) {
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].index == &x) return column_value(i);
        fail(
            "an array index runs on columns only through "
            "call_column()",
            true);
    }

    V node(const TList& l) {
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].list == &l) return column_value(i);
        fail(
            "a list literal runs on columns only through "
            "call_column()",
            true);
    }

    V node(const TQuant& q) {
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].quant == &q)
                return {df::expr_col(static_cast<std::int32_t>(i)), K::BOOL};
        fail(std::string(q.all ? "all" : "any") +
                 "() runs on a list column only through quantify()",
             true);
    }

    V node(const TLookup& l) {
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].lookup == &l) {
                const K k = kind_of(columns_[i].type.id);
                if (k == K::OTHER || k == K::LIST) return null();
                return {df::expr_col(static_cast<std::int32_t>(i)), k};
            }
        fail("a lookup into '" + l.name +
                 "' runs on columns only through lookup()",
             true);
    }

    // The column of `t` when it is a parse_json() call.
    const V* json_call(const Term& t) {
        const auto* c = std::get_if<TCall>(&t.node);
        if (!c || c->fn != Fn::PARSE_JSON) return nullptr;
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].call == c) {
                json_ = {df::expr_col(static_cast<std::int32_t>(i)), K::STR};
                return &json_;
            }
        return nullptr;
    }

   public:
    // `t` itself, or the text column of a parse_json() call.
    V whole(const Term& t) {
        if (const auto* j = json_call(t)) return *j;
        return (*this)(t);
    }

   private:
    V json_;

    V column_value(std::size_t i) {
        if (columns_[i].json)
            fail(
                "parse_json() gives a json value, which only json() "
                "and a whole select or derive item read here",
                true);
        V v{df::expr_col(static_cast<std::int32_t>(i)),
            kind_of(columns_[i].type.id)};
        v.elem = element(columns_[i].type);
        if (v.k == K::OTHER) return null();
        return v;
    }

    V column_call(const TCall& c) {
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].call == &c) return column_value(i);
        fail(std::string(fn_name(c)) +
                 "() runs on columns only through "
                 "call_column()",
             true);
    }

    V node(const TMatch& m) {
        const V v = (*this)(*m.subject);
        if (v.k != K::STR || !m.compiled) return null();
        const Expr t = df::expr_str_pattern(v.e, m.compiled);
        return {m.negated ? df::expr_not(t) : t, K::BOOL};
    }

    // contains() over a list literal: whether `p` equals an item, unknown
    // when `p` is.
    V contains_item(const TList& l, const V& p) {
        if (p.k == K::NUL) return null();
        Expr any;
        for (const auto& item : l.items) {
            const V x = (*this)(*item);
            if (x.k == K::NUL || !comparable(p.k, x.k)) continue;
            const Expr hit = df::expr_coalesce(
                {compare(CmpOp::Eq, p, x).e, df::expr_lit_bool(false)});
            any = any.valid() ? df::expr_logical(LogicalOp::Or, any, hit) : hit;
        }
        if (!any.valid()) any = df::expr_lit_bool(false);
        if (p.constant) return {any, K::BOOL};
        return {df::expr_select(df::expr_is_null(p.e, true),
                                df::expr_lit_null(TypeId::Bool), any),
                K::BOOL};
    }

    V arg(const TCall& c, std::size_t i) { return (*this)(*c.args[i]); }

    static const std::string* const_str(const V& v) {
        return v.constant ? std::get_if<std::string>(&v.constant->value)
                          : nullptr;
    }

    static std::optional<std::int64_t> const_int(const V& v) {
        if (!v.constant) return std::nullopt;
        if (const auto* i = std::get_if<std::int64_t>(&v.constant->value))
            return *i;
        if (const auto* u = std::get_if<std::uint64_t>(&v.constant->value))
            if (*u <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max()))
                return static_cast<std::int64_t>(*u);
        return std::nullopt;
    }

    static std::string_view fn_name(const TCall& c) {
        return fn_info(c.fn).name;
    }

    static V text(const V& v) {
        switch (v.k) {
            case K::STR:
                return v;
            case K::INT:
            case K::UINT:
            case K::FLOAT:
            case K::BOOL:
                return {df::expr_convert(df::ConvertOp::String, v.e), K::STR};
            default:
                return null();
        }
    }

    // `cond` as a Bool that is false where it is unknown.
    static Expr yes(const V& cond) {
        if (cond.k != K::BOOL) return df::expr_lit_bool(false);
        return df::expr_coalesce({cond.e, df::expr_lit_bool(false)});
    }

    V choose(const V& cond, const V& a, const V& b) {
        const K k = common({a, b}, "if()");
        if (k == K::NUL) return null();
        return {df::expr_select(yes(cond), as(a, k), as(b, k)), k};
    }

    V node(const TCall& c) {
        switch (c.fn) {
            case Fn::EXISTS:
                return present(*c.args[0]);
            case Fn::COALESCE: {
                std::vector<V> vs;
                for (const auto& a : c.args) vs.push_back((*this)(*a));
                return coalesce(std::move(vs), "coalesce()");
            }
            case Fn::IF:
                return choose(arg(c, 0), arg(c, 1), arg(c, 2));
            case Fn::CASE: {
                V out = arg(c, c.args.size() - 1);
                for (std::size_t i = c.args.size() - 1; i >= 2; i -= 2)
                    out = choose(arg(c, i - 2), arg(c, i - 1), out);
                return out;
            }
            case Fn::ABS: {
                const V v = arg(c, 0);
                if (!numeric(v.k)) return null();
                if (v.k == K::FLOAT)
                    return {df::expr_unary(df::UnaryOp::Abs, v.e), K::FLOAT};
                if (v.k == K::UINT) return v;
                return {
                    df::expr_select(df::expr_cmp(CmpOp::Lt, v.e,
                                                 df::detail::expr_scalar_i(0)),
                                    df::expr_neg(v.e), v.e),
                    K::INT};
            }
            case Fn::FLOOR:
            case Fn::CEIL: {
                const V v = arg(c, 0);
                if (!numeric(v.k)) return null();
                if (v.k != K::FLOAT) return v;
                return {df::expr_unary(c.fn == Fn::FLOOR ? df::UnaryOp::Floor
                                                         : df::UnaryOp::Ceil,
                                       v.e),
                        K::FLOAT};
            }
            case Fn::ROUND: {
                const V v = arg(c, 0);
                std::int64_t digits = 0;
                if (c.args.size() > 1) {
                    const V d = arg(c, 1);
                    if (!numeric(d.k)) return null();
                    if (!numeric(v.k)) return null();
                    const auto n = const_int(d);
                    if (!n)
                        return {df::expr_round_col(v.e, d.e),
                                v.k == K::FLOAT ? K::FLOAT : K::INT};
                    digits = *n;
                }
                if (!numeric(v.k)) return null();
                return {df::expr_round(v.e, digits),
                        v.k == K::FLOAT ? K::FLOAT : K::INT};
            }
            case Fn::MIN:
            case Fn::MAX: {
                if (c.args.size() == 1) {
                    if (arg(c, 0).k == K::LIST)
                        no_kernel("min() and max() of a list");
                    return null();
                }
                std::vector<Expr> args;
                K k = K::NUL;
                for (const auto& a : c.args) {
                    const V v = (*this)(*a);
                    if (v.k == K::NUL) return null();
                    const bool same = k == K::NUL || comparable(k, v.k);
                    if (!same || v.k == K::BOOL || v.k == K::LIST ||
                        v.k == K::OTHER)
                        return null();
                    k = k == K::NUL || k == v.k ? v.k : K::FLOAT;
                    args.push_back(v.e);
                }
                return {df::expr_extreme(args, c.fn == Fn::MIN), k};
            }
            case Fn::LOG:
            case Fn::EXP: {
                const V v = arg(c, 0);
                if (!numeric(v.k)) return null();
                return {c.fn == Fn::LOG ? df::expr_log(v.e)
                                        : df::expr_unary(df::UnaryOp::Exp, v.e),
                        K::FLOAT};
            }
            case Fn::POW: {
                const V a = arg(c, 0);
                const V b = arg(c, 1);
                if (!numeric(a.k) || !numeric(b.k)) return null();
                return {df::expr_pow(a.e, b.e), K::FLOAT};
            }
            case Fn::LEN: {
                const V v = arg(c, 0);
                if (v.k == K::STR) return {df::expr_str_len(v.e, true), K::INT};
                if (v.k == K::LIST) return {df::expr_list_len(v.e), K::INT};
                return null();
            }
            case Fn::CONCAT: {
                std::vector<Expr> args;
                for (const auto& a : c.args) {
                    const V t = text((*this)(*a));
                    if (t.k == K::NUL) return null();
                    args.push_back(t.e);
                }
                if (args.size() == 1) return {args.front(), K::STR};
                return {df::expr_concat(args), K::STR};
            }
            case Fn::LOWER:
            case Fn::UPPER:
            case Fn::TRIM: {
                const V v = arg(c, 0);
                if (v.k != K::STR) return null();
                return {
                    df::expr_str_map(c.fn == Fn::LOWER   ? df::StrMapOp::Lower
                                     : c.fn == Fn::UPPER ? df::StrMapOp::Upper
                                                         : df::StrMapOp::Strip,
                                     v.e),
                    K::STR};
            }
            case Fn::STARTS_WITH:
            case Fn::ENDS_WITH: {
                const V v = arg(c, 0);
                const V p = arg(c, 1);
                if (v.k != K::STR || p.k != K::STR) return null();
                const auto op = c.fn == Fn::STARTS_WITH
                                    ? df::StrPredOp::StartsWith
                                    : df::StrPredOp::EndsWith;
                if (const auto* n = const_str(p))
                    return {df::expr_str_pred(op, v.e, *n), K::BOOL};
                return {df::expr_str_pred_col(op, v.e, p.e), K::BOOL};
            }
            case Fn::CONTAINS: {
                if (const auto* l = std::get_if<TList>(&c.args[0]->node))
                    return contains_item(*l, arg(c, 1));
                const V v = arg(c, 0);
                const V p = arg(c, 1);
                if (p.k == K::NUL) return null();
                if (v.k == K::STR) {
                    if (p.k != K::STR) return null();
                    if (const auto* n = const_str(p))
                        return {
                            df::expr_str_pred(df::StrPredOp::Contains, v.e, *n),
                            K::BOOL};
                    return {df::expr_str_pred_col(df::StrPredOp::Contains, v.e,
                                                  p.e),
                            K::BOOL};
                }
                if (v.k != K::LIST) return null();
                if (const auto i = const_int(p))
                    return {df::expr_list_contains(
                                v.e, df::detail::expr_scalar_i(*i)),
                            K::BOOL};
                if (p.constant && p.k == K::FLOAT)
                    return {df::expr_list_contains(
                                v.e, df::detail::expr_scalar_d(
                                         std::get<double>(p.constant->value))),
                            K::BOOL};
                no_kernel("contains() of a list with a non-numeric literal");
            }
            case Fn::SUBSTR: {
                const V v = arg(c, 0);
                const V s = arg(c, 1);
                if (v.k != K::STR || s.k == K::NUL) return null();
                if (!numeric(s.k)) return null();
                std::optional<V> l;
                if (c.args.size() > 2) {
                    l = arg(c, 2);
                    if (l->k == K::NUL || !numeric(l->k)) return null();
                }
                const auto start = const_int(s);
                const auto n = l ? const_int(*l) : std::nullopt;
                if (!start || (l && !n)) {
                    return {
                        df::expr_str_substr_col(v.e, s.e, l ? &l->e : nullptr),
                        K::STR};
                }
                std::int64_t len = -1;
                if (l) {
                    if (*n < 0) return null();
                    len = *n;
                }
                if (*start < 0) return null();
                return {df::expr_str_substr(v.e, *start, len), K::STR};
            }
            case Fn::REPLACE: {
                const V v = arg(c, 0);
                const V from = arg(c, 1);
                const V to = arg(c, 2);
                if (v.k != K::STR || from.k != K::STR || to.k != K::STR)
                    return null();
                const std::string* f = const_str(from);
                const std::string* t = const_str(to);
                if (!f || !t)
                    return {df::expr_str_replace_col(v.e, from.e, to.e),
                            K::STR};
                if (f->empty()) return v;
                return {df::expr_str_replace(v.e, *f, *t, true), K::STR};
            }
            case Fn::FIRST:
            case Fn::LAST: {
                const V v = arg(c, 0);
                if (v.k != K::LIST || v.elem == K::OTHER) return null();
                return {df::expr_list_get(v.e, c.fn == Fn::FIRST ? 0 : -1),
                        v.elem};
            }
            case Fn::SUM: {
                const V v = arg(c, 0);
                if (v.k != K::LIST || !numeric(v.elem)) return null();
                return {df::expr_list_sum(v.e),
                        v.elem == K::FLOAT ? K::FLOAT : K::INT};
            }
            case Fn::JSON: {
                if (const auto* j = json_call(*c.args[0])) return *j;
                if (const auto* f = std::get_if<TField>(&c.args[0]->node)) {
                    std::size_t consumed = 0;
                    const auto i = field_column(*f, consumed);
                    if (i && consumed == f->steps.size() && columns_[*i].json)
                        return {df::expr_col(static_cast<std::int32_t>(*i)),
                                K::STR};
                }
                const V v = arg(c, 0);
                if (v.k == K::NUL)
                    return std::holds_alternative<TField>(c.args[0]->node)
                               ? null()
                               : V{df::expr_lit_str("null"), K::STR};
                if (v.k == K::LIST || v.k == K::OTHER)
                    no_kernel("json() of a list");
                return {df::expr_convert(df::ConvertOp::Json, v.e), K::STR};
            }
            case Fn::TYPE: {
                const V v = arg(c, 0);
                std::string_view name;
                switch (v.k) {
                    case K::INT:
                    case K::UINT:
                    case K::FLOAT:
                        name = "number";
                        break;
                    case K::STR:
                        name = "string";
                        break;
                    case K::BOOL:
                        name = "bool";
                        break;
                    case K::LIST:
                        name = "array";
                        break;
                    default:
                        return {df::expr_lit_str("null"), K::STR};
                }
                return {df::expr_select(df::expr_is_null(v.e),
                                        df::expr_lit_str("null"),
                                        df::expr_lit_str(name)),
                        K::STR};
            }
            case Fn::INT:
            case Fn::FLOAT: {
                const V v = arg(c, 0);
                if (v.k == K::NUL || v.k == K::LIST || v.k == K::OTHER)
                    return null();
                const bool i = c.fn == Fn::INT;
                if (i && v.k == K::INT) return v;
                if (!i && v.k == K::FLOAT) return v;
                return {df::expr_convert(
                            i ? df::ConvertOp::Int : df::ConvertOp::Float, v.e),
                        i ? K::INT : K::FLOAT};
            }
            case Fn::STRING:
                return text(arg(c, 0));
            case Fn::SLICE:
            case Fn::FLATTEN:
            case Fn::KEYS:
            case Fn::VALUES:
            case Fn::SPLIT:
            case Fn::PARSE_JSON:
            case Fn::INDEX_OF:
            case Fn::SORT:
            case Fn::UNIQUE:
            case Fn::JOIN:
                return column_call(c);
            case Fn::EXTRACT: {
                const V v = arg(c, 0);
                if (v.k != K::STR || !c.pattern) return null();
                std::int64_t group = 0;
                if (c.args.size() > 2) {
                    const V g = arg(c, 2);
                    if (g.k == K::NUL) return null();
                    if (!numeric(g.k)) return null();
                    const auto n = const_int(g);
                    if (!n)
                        return {df::expr_str_extract_col(v.e, c.pattern, g.e),
                                K::STR};
                    if (*n < 0) return null();
                    group = *n;
                }
                return {df::expr_str_extract(v.e, c.pattern, group), K::STR};
            }
            case Fn::REGEX_REPLACE: {
                const V v = arg(c, 0);
                if (v.k != K::STR || !c.pattern || !c.substitution)
                    return null();
                return {
                    df::expr_str_regex_replace(v.e, c.pattern, *c.substitution),
                    K::STR};
            }
            case Fn::DATE_PART: {
                const V v = arg(c, 0);
                if (!numeric(v.k)) return null();
                return {df::expr_date_part(v.e, c.part, c.ns_per_unit), K::INT};
            }
            case Fn::FORMAT_TIME: {
                const V v = arg(c, 0);
                if (!numeric(v.k)) return null();
                const auto* fmt = std::get_if<TConst>(&c.args[1]->node);
                return {
                    df::expr_format_time(v.e, std::get<std::string>(fmt->value),
                                         c.ns_per_unit),
                    K::STR};
            }
        }
        return null();
    }
};

}  // namespace

dftracer::utils::expected<dataframe::Expr, VectorizeError> vectorize(
    const Term& t, const std::vector<VectorColumn>& columns,
    bool args_fallback) {
    try {
        V v = Vectorizer(columns, args_fallback).whole(t);
        if (v.k == K::NUL) return df::expr_lit_null(TypeId::Bool);
        return std::move(v.e);
    } catch (const Failure& f) {
        return dftracer::utils::unexpected(f.error);
    }
}

bool is_null_term(const Term& t, const std::vector<VectorColumn>& columns,
                  bool args_fallback) {
    try {
        return Vectorizer(columns, args_fallback).whole(t).k == K::NUL;
    } catch (const Failure&) {
        return false;
    }
}

namespace {

// The key columns of `l` for `rows` rows: each key term evaluated, FLAT.
std::vector<df::Series> key_columns(
    const TLookup& l, const std::vector<const df::Series*>& inputs,
    const std::vector<VectorColumn>& columns, std::int64_t rows,
    bool args_fallback) {
    std::vector<df::Series> out;
    for (const auto& k : l.keys) {
        const V v = Vectorizer(columns, args_fallback)(*k);
        out.push_back(v.k == K::NUL || v.k == K::OTHER || v.k == K::LIST
                          ? df::Series::nulls(TypeId::Bool, rows)
                          : df::eval(v.e, inputs).materialize());
    }
    return out;
}

const LookupTable& bound(const TLookup& l) {
    if (!l.slot || !l.slot->table)
        throw Failure{{"'" + l.name + "' was read before its rows were bound"}};
    return *l.slot->table;
}

}  // namespace

dftracer::utils::expected<dataframe::Series, VectorizeError> lookup(
    const TLookup& l, const std::vector<const dataframe::Series*>& inputs,
    const std::vector<VectorColumn>& columns, bool args_fallback) {
    const std::int64_t rows = inputs.empty() ? 0 : inputs.front()->length();
    try {
        bound(l);
        const std::vector<df::Series> keys =
            key_columns(l, inputs, columns, rows, args_fallback);
        std::vector<const df::Series*> ptrs;
        for (const auto& k : keys) ptrs.push_back(&k);
        return lookup_column(l, ptrs, rows);
    } catch (const Failure& f) {
        return dftracer::utils::unexpected(f.error);
    }
}

dftracer::utils::expected<dataframe::Series, VectorizeError> quantify(
    const TQuant& q, const std::vector<const dataframe::Series*>& inputs,
    const std::vector<VectorColumn>& columns, bool args_fallback) {
    const std::int64_t rows = inputs.empty() ? 0 : inputs.front()->length();
    const df::Series unknown = df::Series::nulls(TypeId::Bool, rows);
    try {
        // Each row's elements are values[starts[r], starts[r + 1]).
        df::Series values;
        std::vector<std::int64_t> starts;
        std::vector<std::uint8_t> present(static_cast<std::size_t>(rows), 0);
        if (const auto* l = std::get_if<TLookup>(&q.subject->node);
            l && l->all) {
            const LookupTable& t = bound(*l);
            const std::vector<df::Series> keys =
                key_columns(*l, inputs, columns, rows, args_fallback);
            std::vector<const df::Series*> ptrs;
            for (const auto& k : keys) ptrs.push_back(&k);
            Matches m = match(t, ptrs, rows);
            const auto n = static_cast<std::int64_t>(m.rows.size());
            values = t.value ? t.frame->columns[*t.value].take(m.rows)
                             : df::Series::nulls(TypeId::String, n);
            starts = std::move(m.offsets);
            for (std::int64_t r = 0; r < rows; ++r)
                present[static_cast<std::size_t>(r)] =
                    starts[static_cast<std::size_t>(r) + 1] >
                    starts[static_cast<std::size_t>(r)];
        } else {
            const V subject = Vectorizer(columns, args_fallback)(*q.subject);
            if (subject.k != K::LIST) return unknown.share();
            df::Series list = df::eval(subject.e, inputs).materialize();
            const std::int32_t* off = list.offsets();
            if (!off) return unknown.share();
            const df::Series child = list.child(0);
            const auto n = static_cast<std::int64_t>(off[rows] - off[0]);
            if (off[0] == 0 && child.length() == n) {
                values = child.share();
            } else {
                std::vector<std::int64_t> at(static_cast<std::size_t>(n));
                std::iota(at.begin(), at.end(), std::int64_t{off[0]});
                values = child.take(at);
            }
            for (std::int64_t r = 0; r <= rows; ++r)
                starts.push_back(off[r] - off[0]);
            for (std::int64_t r = 0; r < rows; ++r)
                present[static_cast<std::size_t>(r)] = !list.is_null(r);
        }
        std::vector<std::int64_t> parent;
        parent.reserve(static_cast<std::size_t>(starts.back()));
        for (std::int64_t r = 0; r < rows; ++r)
            for (std::int64_t e = starts[static_cast<std::size_t>(r)];
                 e < starts[static_cast<std::size_t>(r) + 1]; ++e)
                parent.push_back(r);
        const auto n = static_cast<std::int64_t>(parent.size());

        std::vector<df::Series> owned;
        std::vector<VectorColumn> cols;
        owned.push_back(std::move(values));
        cols.push_back({".", owned.back().data_type()});
        if (owned.front().type() == TypeId::Struct)
            for (std::int64_t i = 0; i < owned.front().num_children(); ++i) {
                df::Series field = owned.front().child(i);
                cols.push_back(
                    {"." + owned.front().field_name(i), field.data_type()});
                owned.push_back(std::move(field));
            }
        std::vector<std::string> read;
        for_each_term_field(*q.cond,
                            [&](const TField& f) { read.push_back(f.base); });
        auto reads = [&](const VectorColumn& c) {
            if (c.quant || c.lookup || c.call) return true;
            for (const auto& b : read)
                if (b == c.name || (args_fallback && "args." + b == c.name) ||
                    (b.size() > c.name.size() && b.starts_with(c.name) &&
                     (b[c.name.size()] == '.' || b[c.name.size()] == '[')))
                    return true;
            return false;
        };
        for (std::size_t i = 0; i < columns.size(); ++i) {
            cols.push_back(columns[i]);
            owned.push_back(reads(columns[i]) ? inputs[i]->take(parent)
                                              : inputs[i]->share());
        }
        std::vector<const df::Series*> frame;
        for (const auto& c : owned) frame.push_back(&c);
        const V cond = Vectorizer(cols, args_fallback)(*q.cond);
        std::vector<std::uint8_t> yes(static_cast<std::size_t>(n), 0);
        std::vector<std::uint8_t> known(static_cast<std::size_t>(n), 0);
        if (cond.k == K::BOOL && n > 0) {
            const df::Series t = df::eval(cond.e, frame).materialize();
            const auto* bits = t.data<std::uint8_t>();
            for (std::int64_t e = 0; e < n; ++e) {
                if (t.is_null(e)) continue;
                known[static_cast<std::size_t>(e)] = 1;
                yes[static_cast<std::size_t>(e)] =
                    (bits[e >> 3] >> (e & 7)) & 1;
            }
        }
        std::vector<std::uint8_t> out((static_cast<std::size_t>(rows) + 7) / 8,
                                      0);
        std::vector<std::uint8_t> valid(
            (static_cast<std::size_t>(rows) + 7) / 8, 0);
        for (std::int64_t r = 0; r < rows; ++r) {
            if (!present[static_cast<std::size_t>(r)]) continue;
            bool hit = false;
            bool unsure = false;
            for (std::int64_t e = starts[static_cast<std::size_t>(r)];
                 e < starts[static_cast<std::size_t>(r) + 1]; ++e) {
                const auto at = static_cast<std::size_t>(e);
                if (!known[at])
                    unsure = true;
                else if ((yes[at] != 0) != q.all)
                    hit = true;
            }
            if (!hit && unsure) continue;
            const auto bit = static_cast<std::uint8_t>(1u << (r & 7));
            if (hit != q.all) out[static_cast<std::size_t>(r) >> 3] |= bit;
            valid[static_cast<std::size_t>(r) >> 3] |= bit;
        }
        return df::Series::flat(TypeId::Bool, out.data(), rows, valid.data());
    } catch (const Failure& f) {
        return dftracer::utils::unexpected(f.error);
    }
}

dftracer::utils::expected<dataframe::Expr, VectorizeError> vectorize_condition(
    const Term& t, const std::vector<VectorColumn>& columns,
    bool args_fallback) {
    try {
        V v = Vectorizer(columns, args_fallback)(t);
        if (v.k != K::BOOL) return df::expr_lit_null(TypeId::Bool);
        return std::move(v.e);
    } catch (const Failure& f) {
        return dftracer::utils::unexpected(f.error);
    }
}

namespace {

bool is_list_type(const df::DataType& t) {
    return (t.id == TypeId::List || t.id == TypeId::LargeList) &&
           !t.fields.empty();
}

// A LIST column's offsets and elements.
struct ListParts {
    df::Series list;
    df::Series child;
    std::vector<std::int64_t> off;
};

std::optional<ListParts> list_parts(const df::Series& s) {
    ListParts p{s.is_flat() ? s.share() : s.materialize(), {}, {}};
    const std::int64_t n = p.list.length();
    if (const auto* o = p.list.offsets()) {
        p.off.assign(o, o + n + 1);
    } else if (const auto* o64 = p.list.offsets64()) {
        p.off.assign(o64, o64 + n + 1);
    } else {
        return std::nullopt;
    }
    p.child = p.list.child(0);
    return p;
}

// A LIST column of `rows[offsets[r], offsets[r + 1])` of `values`, null
// where `valid` is false.
df::Series make_list(const std::vector<std::int64_t>& offsets,
                     df::Series values, const std::vector<bool>& valid) {
    std::vector<std::int32_t> off(offsets.begin(), offsets.end());
    df::Series out = df::Series::list(off, std::move(values));
    if (std::all_of(valid.begin(), valid.end(), [](bool v) { return v; }))
        return out;
    std::vector<std::int64_t> rows(valid.size());
    for (std::size_t r = 0; r < valid.size(); ++r)
        rows[r] = valid[r] ? static_cast<std::int64_t>(r) : -1;
    return out.take(rows);
}

df::Series flat_of(const df::Series& s) {
    return s.is_flat() ? s.share() : s.materialize();
}

std::optional<std::int64_t> int_at(const df::Series& s, std::int64_t r) {
    if (s.is_null(r)) return std::nullopt;
    return s.data<std::int64_t>()[r];
}

std::int64_t clamp_at(std::int64_t i, std::int64_t size) {
    if (i < 0) return std::max<std::int64_t>(0, size + i);
    return std::min(i, size);
}

df::Series slice_column(const std::vector<df::Series>& args) {
    auto p = list_parts(args[0]);
    const std::int64_t n = args[0].length();
    const df::Series from = args[1].cast(TypeId::Int64).materialize();
    const df::Series to = args.size() > 2
                              ? args[2].cast(TypeId::Int64).materialize()
                              : df::Series::nulls(TypeId::Int64, n);
    std::vector<std::int64_t> offsets{0};
    std::vector<std::int64_t> take;
    std::vector<bool> valid(static_cast<std::size_t>(n), false);
    for (std::int64_t r = 0; r < n; ++r) {
        const auto b = int_at(from, r);
        const auto e = int_at(to, r);
        if (!p->list.is_null(r) && b && (e || args.size() < 3)) {
            const std::int64_t lo = p->off[static_cast<std::size_t>(r)];
            const std::int64_t size =
                p->off[static_cast<std::size_t>(r) + 1] - lo;
            const std::int64_t first = clamp_at(*b, size);
            const std::int64_t last = e ? clamp_at(*e, size) : size;
            for (std::int64_t i = first; i < last; ++i) take.push_back(lo + i);
            valid[static_cast<std::size_t>(r)] = true;
        }
        offsets.push_back(static_cast<std::int64_t>(take.size()));
    }
    return make_list(offsets, p->child.take(take), valid);
}

df::Series flatten_column(const df::Series& arg) {
    auto p = list_parts(arg);
    const std::int64_t n = arg.length();
    auto inner = list_parts(p->child);
    if (!inner) return p->list.share();
    std::vector<std::int64_t> offsets{0};
    std::vector<std::int64_t> take;
    std::vector<bool> valid(static_cast<std::size_t>(n), false);
    for (std::int64_t r = 0; r < n; ++r) {
        if (!p->list.is_null(r)) {
            valid[static_cast<std::size_t>(r)] = true;
            for (std::int64_t e = p->off[static_cast<std::size_t>(r)];
                 e < p->off[static_cast<std::size_t>(r) + 1]; ++e) {
                if (inner->list.is_null(e)) {
                    take.push_back(-1);
                    continue;
                }
                for (std::int64_t x = inner->off[static_cast<std::size_t>(e)];
                     x < inner->off[static_cast<std::size_t>(e) + 1]; ++x)
                    take.push_back(x);
            }
        }
        offsets.push_back(static_cast<std::int64_t>(take.size()));
    }
    return make_list(offsets, inner->child.take(take), valid);
}

// The object's fields as (key, column), in the byte order of the keys.
std::vector<std::pair<std::string, df::Series>> object_fields(
    const std::vector<df::Series>& args, const std::vector<std::string>& keys) {
    std::vector<std::pair<std::string, df::Series>> out;
    if (args.size() == 1 && args[0].type() == TypeId::Struct) {
        const df::Series st = flat_of(args[0]);
        for (std::int64_t i = 0; i < st.num_children(); ++i)
            out.emplace_back(st.field_name(i), flat_of(st.child(i)));
    } else {
        for (std::size_t i = 0; i < args.size(); ++i)
            out.emplace_back(keys[i], flat_of(args[i]));
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    return out;
}

df::Series members_column(const std::vector<df::Series>& args,
                          const std::vector<std::string>& keys, bool names,
                          const df::DataType& type) {
    const std::int64_t n = args.empty() ? 0 : args[0].length();
    const auto fields = object_fields(args, keys);
    const bool whole = args.size() == 1 && args[0].type() == TypeId::Struct;
    std::vector<std::int64_t> offsets{0};
    std::vector<std::int64_t> take;
    std::vector<bool> valid(static_cast<std::size_t>(n), false);
    for (std::int64_t r = 0; r < n; ++r) {
        const std::size_t before = take.size();
        if (!whole || !args[0].is_null(r))
            for (std::size_t k = 0; k < fields.size(); ++k)
                if (!fields[k].second.is_null(r))
                    take.push_back(names
                                       ? static_cast<std::int64_t>(k)
                                       : static_cast<std::int64_t>(k) * n + r);
        valid[static_cast<std::size_t>(r)] =
            whole ? !args[0].is_null(r) : take.size() > before;
        offsets.push_back(static_cast<std::int64_t>(take.size()));
    }
    if (names) {
        std::vector<std::string> all;
        for (const auto& f : fields) all.push_back(f.first);
        return make_list(offsets, df::Series::strings(all).take(take), valid);
    }
    const TypeId elem = type.fields.front().type.id;
    std::vector<df::DataFrame> frames(fields.size());
    std::vector<const df::DataFrame*> ptrs;
    for (std::size_t k = 0; k < fields.size(); ++k) {
        frames[k].names.push_back("v");
        frames[k].columns.push_back(fields[k].second.type() == elem
                                        ? fields[k].second.share()
                                        : fields[k].second.cast(elem));
        ptrs.push_back(&frames[k]);
    }
    df::Series values = df::concat(ptrs).columns.front().take(take);
    return make_list(offsets, std::move(values), valid);
}

df::Series parse_column(const df::Series& arg) {
    const df::Series s = flat_of(arg);
    const std::int64_t n = s.length();
    dftracer::utils::json::RecordParser parser;
    std::vector<std::string> text(static_cast<std::size_t>(n));
    std::vector<std::uint8_t> bits(static_cast<std::size_t>((n + 7) / 8), 0);
    for (std::int64_t r = 0; r < n; ++r) {
        if (s.is_null(r)) continue;
        const std::string_view in = s.string_at(r);
        simdjson::dom::element el;
        if (parser.parse(in.data(), in.size()).get(el) != simdjson::SUCCESS)
            continue;
        json::append_canonical_json(text[static_cast<std::size_t>(r)], el);
        bits[static_cast<std::size_t>(r) >> 3] |=
            static_cast<std::uint8_t>(1u << (r & 7));
    }
    const std::vector<std::string_view> views(text.begin(), text.end());
    return df::Series::strings(views, bits.data());
}

df::Series split_column(const df::Series& arg, const df::Series& sep_arg) {
    const df::Series s = flat_of(arg);
    const df::Series sep = flat_of(sep_arg);
    const std::int64_t n = s.length();
    std::vector<std::int64_t> offsets{0};
    std::vector<std::string_view> parts;
    std::vector<bool> valid(static_cast<std::size_t>(n), false);
    for (std::int64_t r = 0; r < n; ++r) {
        if (!s.is_null(r) && !sep.is_null(r) && !sep.string_at(r).empty()) {
            const std::string_view text = s.string_at(r);
            const std::string_view p = sep.string_at(r);
            std::size_t at = 0;
            for (;;) {
                const std::size_t hit = text.find(p, at);
                parts.push_back(text.substr(at, hit - at));
                if (hit == std::string_view::npos) break;
                at = hit + p.size();
            }
            valid[static_cast<std::size_t>(r)] = true;
        }
        offsets.push_back(static_cast<std::int64_t>(parts.size()));
    }
    return make_list(offsets, df::Series::strings(parts), valid);
}

using Cell = std::variant<Number, bool, std::string_view>;

std::optional<int> cmp_cell(const Cell& a, const Cell& b) {
    if (a.index() != b.index()) return std::nullopt;
    if (const auto* x = std::get_if<Number>(&a))
        return compare_numbers(*x, std::get<Number>(b));
    if (const auto* x = std::get_if<bool>(&a))
        return static_cast<int>(*x) - static_cast<int>(std::get<bool>(b));
    const int c =
        std::get<std::string_view>(a).compare(std::get<std::string_view>(b));
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

bool scalar_kind(K k) { return numeric(k) || k == K::STR || k == K::BOOL; }

// The cells of a column of one scalar kind, read without converting a row.
class Cells {
   public:
    explicit Cells(const df::Series& s)
        : k_(kind_of(s.type())), s_(prepare(s, k_)) {}

    bool null(std::int64_t i) const { return s_.is_null(i); }

    Cell at(std::int64_t i) const {
        switch (k_) {
            case K::INT:
                return Number{s_.data<std::int64_t>()[i]};
            case K::UINT:
                return Number{s_.data<std::uint64_t>()[i]};
            case K::FLOAT:
                return Number{s_.data<double>()[i]};
            case K::BOOL:
                return ((s_.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1) != 0;
            default:
                return s_.string_at(i);
        }
    }

   private:
    K k_;
    df::Series s_;

    static df::Series prepare(const df::Series& s, K k) {
        if (k == K::INT) return flat_of(s.cast(TypeId::Int64));
        if (k == K::FLOAT) return flat_of(s.cast(TypeId::Float64));
        return flat_of(s);
    }
};

// The element at each row's index, 0-based and from the end when negative.
df::Series index_column(const std::vector<df::Series>& args) {
    auto p = list_parts(args[0]);
    const std::int64_t n = args[0].length();
    const Cells at(args[1]);
    std::vector<std::int64_t> take(static_cast<std::size_t>(n), -1);
    for (std::int64_t r = 0; r < n; ++r) {
        if (p->list.is_null(r) || at.null(r)) continue;
        const Number num = std::get<Number>(at.at(r));
        std::int64_t i;
        if (const auto* s = std::get_if<std::int64_t>(&num)) {
            i = *s;
        } else {
            const std::uint64_t u = std::get<std::uint64_t>(num);
            if (u > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()))
                continue;
            i = static_cast<std::int64_t>(u);
        }
        const std::int64_t lo = p->off[static_cast<std::size_t>(r)];
        const std::int64_t size = p->off[static_cast<std::size_t>(r) + 1] - lo;
        if (i < 0) i += size;
        if (i >= 0 && i < size) take[static_cast<std::size_t>(r)] = lo + i;
    }
    return p->child.take(take);
}

df::Series index_of_column(const df::Series& arg, const df::Series& value) {
    auto p = list_parts(arg);
    const std::int64_t n = arg.length();
    const Cells elems(p->child);
    const Cells v(value);
    std::vector<std::int64_t> out(static_cast<std::size_t>(n), 0);
    std::vector<std::uint8_t> bits(static_cast<std::size_t>((n + 7) / 8), 0);
    for (std::int64_t r = 0; r < n; ++r) {
        if (p->list.is_null(r) || v.null(r)) continue;
        const Cell x = v.at(r);
        const std::int64_t lo = p->off[static_cast<std::size_t>(r)];
        const std::int64_t hi = p->off[static_cast<std::size_t>(r) + 1];
        for (std::int64_t e = lo; e < hi; ++e) {
            if (elems.null(e)) continue;
            const auto o = cmp_cell(elems.at(e), x);
            if (!o || *o != 0) continue;
            out[static_cast<std::size_t>(r)] = e - lo;
            bits[static_cast<std::size_t>(r) >> 3] |=
                static_cast<std::uint8_t>(1u << (r & 7));
            break;
        }
    }
    return df::Series::flat(TypeId::Int64, out.data(), n, bits.data());
}

// Each row's elements ascending, nulls last; null where a present element
// does not compare with the first.
df::Series sort_column(const df::Series& arg) {
    auto p = list_parts(arg);
    const std::int64_t n = arg.length();
    const Cells cells(p->child);
    std::vector<std::int64_t> offsets{0};
    std::vector<std::int64_t> take;
    std::vector<std::int64_t> present;
    std::vector<bool> valid(static_cast<std::size_t>(n), false);
    for (std::int64_t r = 0; r < n; ++r) {
        if (!p->list.is_null(r)) {
            const std::int64_t lo = p->off[static_cast<std::size_t>(r)];
            const std::int64_t hi = p->off[static_cast<std::size_t>(r) + 1];
            present.clear();
            for (std::int64_t e = lo; e < hi; ++e)
                if (!cells.null(e)) present.push_back(e);
            bool ok = true;
            for (std::size_t i = 1; ok && i < present.size(); ++i)
                ok = cmp_cell(cells.at(present[i]), cells.at(present[0]))
                         .has_value();
            if (ok) {
                std::stable_sort(present.begin(), present.end(),
                                 [&](std::int64_t a, std::int64_t b) {
                                     return *cmp_cell(cells.at(a),
                                                      cells.at(b)) < 0;
                                 });
                take.insert(take.end(), present.begin(), present.end());
                take.insert(take.end(),
                            static_cast<std::size_t>(hi - lo) - present.size(),
                            std::int64_t{-1});
                valid[static_cast<std::size_t>(r)] = true;
            }
        }
        offsets.push_back(static_cast<std::int64_t>(take.size()));
    }
    return make_list(offsets, p->child.take(take), valid);
}

// The first of each run of equal elements, in order; nulls are equal.
df::Series unique_column(const df::Series& arg) {
    auto p = list_parts(arg);
    const std::int64_t n = arg.length();
    const Cells cells(p->child);
    auto same = [&](std::int64_t a, std::int64_t b) {
        if (cells.null(a) || cells.null(b))
            return cells.null(a) && cells.null(b);
        const auto o = cmp_cell(cells.at(a), cells.at(b));
        return o && *o == 0;
    };
    std::vector<std::int64_t> offsets{0};
    std::vector<std::int64_t> take;
    std::vector<bool> valid(static_cast<std::size_t>(n), false);
    for (std::int64_t r = 0; r < n; ++r) {
        if (!p->list.is_null(r)) {
            valid[static_cast<std::size_t>(r)] = true;
            const std::size_t first = take.size();
            for (std::int64_t e = p->off[static_cast<std::size_t>(r)];
                 e < p->off[static_cast<std::size_t>(r) + 1]; ++e) {
                bool seen = false;
                for (std::size_t k = first; !seen && k < take.size(); ++k)
                    seen = same(take[k], e);
                if (!seen) take.push_back(e);
            }
        }
        offsets.push_back(static_cast<std::int64_t>(take.size()));
    }
    return make_list(offsets, p->child.take(take), valid);
}

// The string elements joined by the separator, nulls skipped; null where an
// element is not a string.
df::Series join_column(const df::Series& arg, const df::Series& sep_arg) {
    auto p = list_parts(arg);
    const std::int64_t n = arg.length();
    const df::Series elems = flat_of(p->child);
    const bool strings = kind_of(elems.type()) == K::STR;
    const df::Series sep = flat_of(sep_arg);
    std::string buf;
    std::vector<std::pair<std::size_t, std::size_t>> spans(
        static_cast<std::size_t>(n));
    std::vector<std::uint8_t> bits(static_cast<std::size_t>((n + 7) / 8), 0);
    for (std::int64_t r = 0; r < n; ++r) {
        if (p->list.is_null(r) || sep.is_null(r)) continue;
        const std::size_t begin = buf.size();
        const std::string_view s = sep.string_at(r);
        bool first = true;
        bool ok = true;
        for (std::int64_t e = p->off[static_cast<std::size_t>(r)];
             ok && e < p->off[static_cast<std::size_t>(r) + 1]; ++e) {
            if (elems.is_null(e)) continue;
            if (!strings) {
                ok = false;
                break;
            }
            if (!first) buf += s;
            first = false;
            buf += elems.string_at(e);
        }
        if (!ok) {
            buf.resize(begin);
            continue;
        }
        spans[static_cast<std::size_t>(r)] = {begin, buf.size()};
        bits[static_cast<std::size_t>(r) >> 3] |=
            static_cast<std::uint8_t>(1u << (r & 7));
    }
    std::vector<std::string_view> views;
    views.reserve(spans.size());
    for (const auto& [b, e] : spans) views.emplace_back(buf.data() + b, e - b);
    return df::Series::strings(views, bits.data());
}

// One list per row of the items' values at that row.
df::Series list_column(const std::vector<df::Series>& items, std::int64_t n,
                       const df::DataType& type) {
    const TypeId elem = type.fields.front().type.id;
    const auto m = static_cast<std::int64_t>(items.size());
    std::vector<std::int64_t> offsets(static_cast<std::size_t>(n) + 1, 0);
    for (std::int64_t r = 0; r <= n; ++r)
        offsets[static_cast<std::size_t>(r)] = r * m;
    const std::vector<bool> valid(static_cast<std::size_t>(n), true);
    if (items.empty())
        return make_list(offsets, df::Series::nulls(elem, 0), valid);
    std::vector<df::DataFrame> frames(items.size());
    std::vector<const df::DataFrame*> ptrs;
    for (std::size_t k = 0; k < items.size(); ++k) {
        const df::Series s = items[k].null_count() == n
                                 ? df::Series::nulls(elem, n)
                                 : flat_of(items[k]);
        frames[k].names.push_back("v");
        frames[k].columns.push_back(s.type() == elem ? s.share()
                                                     : s.cast(elem));
        ptrs.push_back(&frames[k]);
    }
    std::vector<std::int64_t> take;
    take.reserve(static_cast<std::size_t>(n * m));
    for (std::int64_t r = 0; r < n; ++r)
        for (std::int64_t k = 0; k < m; ++k) take.push_back(k * n + r);
    return make_list(offsets, df::concat(ptrs).columns.front().take(take),
                     valid);
}

}  // namespace

bool is_column_call(Fn fn) {
    return fn == Fn::SLICE || fn == Fn::FLATTEN || fn == Fn::KEYS ||
           fn == Fn::VALUES || fn == Fn::PARSE_JSON || fn == Fn::SPLIT ||
           fn == Fn::INDEX_OF || fn == Fn::SORT || fn == Fn::UNIQUE ||
           fn == Fn::JOIN;
}

bool is_column_term(const Term& t) {
    if (const auto* c = std::get_if<TCall>(&t.node))
        return is_column_call(c->fn);
    return std::holds_alternative<TIndex>(t.node) ||
           std::holds_alternative<TList>(t.node);
}

std::optional<df::DataType> call_type(const Term& t,
                                      const std::vector<df::DataType>& args) {
    auto integer = [](const df::DataType& d) {
        return kind_of(d.id) == K::INT || kind_of(d.id) == K::UINT;
    };
    if (std::holds_alternative<TIndex>(t.node)) {
        if (!is_list_type(args[0]) || !integer(args[1])) return std::nullopt;
        return args[0].fields.front().type;
    }
    if (std::holds_alternative<TList>(t.node)) {
        if (args.empty()) return df::list_of(df::scalar(TypeId::Int64));
        K k = K::NUL;
        for (const auto& a : args) {
            if (a.id == TypeId::Unknown) continue;
            const K x = kind_of(a.id);
            if (!scalar_kind(x) ||
                (k != K::NUL && k != x && !(numeric(k) && numeric(x))))
                throw std::invalid_argument(
                    "a list after the scan needs items of one type");
            k = k == K::NUL || k == x ? x : K::FLOAT;
        }
        switch (k) {
            case K::INT:
                return df::list_of(df::scalar(TypeId::Int64));
            case K::UINT:
                return df::list_of(df::scalar(TypeId::Uint64));
            case K::FLOAT:
                return df::list_of(df::scalar(TypeId::Float64));
            case K::STR:
                return df::list_of(df::scalar(TypeId::String));
            default:
                return df::list_of(df::scalar(TypeId::Bool));
        }
    }
    const TCall& c = std::get<TCall>(t.node);
    switch (c.fn) {
        case Fn::SLICE:
            if (!is_list_type(args[0])) return std::nullopt;
            for (std::size_t i = 1; i < args.size(); ++i)
                if (!integer(args[i])) return std::nullopt;
            return df::list_of(args[0].fields.front().type);
        case Fn::FLATTEN: {
            if (!is_list_type(args[0])) return std::nullopt;
            const df::DataType& elem = args[0].fields.front().type;
            return df::list_of(is_list_type(elem) ? elem.fields.front().type
                                                  : elem);
        }
        case Fn::KEYS:
            if (args.empty()) return std::nullopt;
            return df::list_of(df::scalar(TypeId::String));
        case Fn::VALUES: {
            std::vector<df::DataType> fields = args;
            if (args.size() == 1 && args[0].id == TypeId::Struct) {
                fields.clear();
                for (const auto& f : args[0].fields) fields.push_back(f.type);
            }
            if (fields.empty()) return std::nullopt;
            K k = kind_of(fields.front().id);
            for (const auto& f : fields) {
                const K x = kind_of(f.id);
                if (x == k) continue;
                if (numeric(x) && numeric(k)) {
                    k = K::FLOAT;
                    continue;
                }
                throw std::invalid_argument(
                    "values() reads fields of different types; convert them "
                    "with int(), float() or string()");
            }
            if (k == K::OTHER || k == K::LIST)
                throw std::invalid_argument(
                    "values() reads a field that holds a list or object");
            return df::list_of(df::scalar(type_of(k)));
        }
        case Fn::PARSE_JSON:
            if (kind_of(args[0].id) != K::STR) return std::nullopt;
            return df::scalar(TypeId::String);
        case Fn::SPLIT:
            if (kind_of(args[0].id) != K::STR || kind_of(args[1].id) != K::STR)
                return std::nullopt;
            return df::list_of(df::scalar(TypeId::String));
        case Fn::INDEX_OF:
            if (!is_list_type(args[0]) ||
                !scalar_kind(kind_of(args[0].fields.front().type.id)) ||
                !scalar_kind(kind_of(args[1].id)))
                return std::nullopt;
            return df::scalar(TypeId::Int64);
        case Fn::SORT:
        case Fn::UNIQUE:
            if (!is_list_type(args[0])) return std::nullopt;
            if (!scalar_kind(kind_of(args[0].fields.front().type.id)))
                throw std::invalid_argument(
                    std::string(fn_info(c.fn).name) +
                    "() reads an array of arrays or objects");
            return df::list_of(args[0].fields.front().type);
        case Fn::JOIN:
            if (!is_list_type(args[0]) || kind_of(args[1].id) != K::STR)
                return std::nullopt;
            return df::scalar(TypeId::String);
        default:
            return std::nullopt;
    }
}

df::Series call_column(const Term& t, const std::vector<df::Series>& args,
                       const std::vector<std::string>& keys,
                       const df::DataType& type, std::int64_t rows) {
    if (std::holds_alternative<TIndex>(t.node)) return index_column(args);
    if (std::holds_alternative<TList>(t.node))
        return list_column(args, rows, type);
    switch (std::get<TCall>(t.node).fn) {
        case Fn::SLICE:
            return slice_column(args);
        case Fn::FLATTEN:
            return flatten_column(args[0]);
        case Fn::KEYS:
        case Fn::VALUES:
            return members_column(args, keys,
                                  std::get<TCall>(t.node).fn == Fn::KEYS, type);
        case Fn::PARSE_JSON:
            return parse_column(args[0]);
        case Fn::SPLIT:
            return split_column(args[0], args[1]);
        case Fn::INDEX_OF:
            return index_of_column(args[0], args[1]);
        case Fn::SORT:
            return sort_column(args[0]);
        case Fn::UNIQUE:
            return unique_column(args[0]);
        case Fn::JOIN:
            return join_column(args[0], args[1]);
        default:
            return df::Series::nulls(type.id, rows);
    }
}

}  // namespace dftracer::utils::duql
