// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dstd/algorithm.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/variant.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/errc.h>
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
#include <utility>
#include <variant>

namespace dftracer::utils::duql {

namespace {

using dataframe::CmpOp;
using dataframe::Expr;
using dataframe::LogicalOp;
using dataframe::TypeId;

// ANY: a column whose type the plan does not know (no catalog); each batch
// gives it its own, and the kernels dispatch on that.
enum class K : std::uint8_t {
    Int,
    Uint,
    Float,
    Str,
    Bool,
    List,
    Nul,
    Other,
    Any
};

struct V {
    Expr e;
    K k = K::Nul;
    const TConst* constant = nullptr;
    K elem = K::Other;  ///< The element kind of a LIST.
};

bool numeric(K k) { return k == K::Int || k == K::Uint || k == K::Float; }

// A value an arithmetic or comparison may take: numeric, or decided per batch.
bool number_like(K k) { return numeric(k) || k == K::Any; }

K kind_of(TypeId t) {
    switch (t) {
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
            return K::Int;
        case TypeId::Uint64:
            return K::Uint;
        case TypeId::Float16:
        case TypeId::Float32:
        case TypeId::Float64:
            return K::Float;
        case TypeId::String:
        case TypeId::LargeString:
            return K::Str;
        case TypeId::Bool:
            return K::Bool;
        case TypeId::List:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
            return K::List;
        default:
            return K::Other;
    }
}

// The kind of a column of type `t`; Unknown is decided per batch.
K column_kind(TypeId t) { return t == TypeId::Unknown ? K::Any : kind_of(t); }

TypeId type_of(K k) {
    switch (k) {
        case K::Uint:
            return TypeId::Uint64;
        case K::Float:
            return TypeId::Float64;
        case K::Str:
            return TypeId::String;
        case K::Bool:
            return TypeId::Bool;
        default:
            return TypeId::Int64;
    }
}

dstd::Unexpected<dstd::Error> unsupported(const dstd::String& message) {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error{DuqlErrc::Unsupported, dstd::StringView(message)});
}

class Vectorizer {
   public:
    Vectorizer(const dstd::Vector<VectorColumn>& columns, bool args_fallback)
        : columns_(columns), args_fallback_(args_fallback) {}

    bool failed() const { return !error_.empty(); }
    const dstd::String& error() const { return error_; }

    V operator()(const Term& t) {
        return std::visit([this](const auto& n) { return this->node(n); },
                          t.node);
    }

   private:
    const dstd::Vector<VectorColumn>& columns_;
    bool args_fallback_;

    // The first unsupported form; the walk goes on with a null value, and
    // the entry points read error() after it.
    mutable dstd::String error_;

    V fail(dstd::String msg) const {
        if (error_.empty()) error_ = std::move(msg);
        return null();
    }

    V checked(dstd::Result<Expr> e, K k) const {
        if (!e) return fail(dstd::String(e.error().message()));
        return {std::move(*e), k};
    }

    V no_kernel(dstd::StringView what) const {
        return fail(dstd::fmt<"{} has no column kernel">(what));
    }

    static V null() { return {}; }
    static V boolean(bool b) { return {dataframe::expr_lit_bool(b), K::Bool}; }
    static Expr as(const V& v, K k) {
        return v.k == K::Nul ? dataframe::expr_lit_null(type_of(k)) : v.e;
    }
    static Expr as_bool(const V& v) {
        return v.k == K::Bool ? v.e : dataframe::expr_lit_null(TypeId::Bool);
    }

    dstd::Optional<std::size_t> column(dstd::StringView name) const {
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].name == name) return i;
        return dstd::nullopt;
    }

    // The column a path names: the path itself or, for a bare name, the
    // `args.` field; with an index step, the list column before it. An
    // element path names `.` or `.<field>`.
    dstd::Optional<std::size_t> field_column(const TField& f,
                                             std::size_t& consumed) const {
        consumed = f.steps.size();
        const bool element = f.root == FieldRoot::Element;
        dstd::String key = element ? "." : "";
        dstd::Optional<std::size_t> best;
        if (element)
            if ((best = column(key))) consumed = 0;
        for (std::size_t i = 0; i < f.steps.size(); ++i) {
            const auto& s = f.steps[i];
            if (s.index) {
                if (key.empty()) return dstd::nullopt;
                key += dstd::fmt<"[{}]">(*s.index);
            } else {
                if (i > 0) key += '.';
                key += s.key;
            }
            auto c = column(key);
            if (!c && !element && args_fallback_)
                c = column(dstd::fmt<"args.{}">(key));
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
                    return {dataframe::expr_lit_bool(v), K::Bool, &c};
                else if constexpr (std::is_same_v<T, std::int64_t>)
                    return {dataframe::expr_lit(v), K::Int, &c};
                else if constexpr (std::is_same_v<T, std::uint64_t>) {
                    if (v > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max()))
                        return no_kernel("an integer literal above int64");
                    return {dataframe::expr_lit(static_cast<std::int64_t>(v)),
                            K::Int, &c};
                } else if constexpr (std::is_same_v<T, double>)
                    return {dataframe::expr_lit(v), K::Float, &c};
                else
                    return {dataframe::expr_lit_str(v), K::Str, &c};
            },
            c.value);
    }

    V node(const TField& f) const {
        std::size_t consumed = 0;
        const auto c = field_column(f, consumed);
        if (!c) return null();
        const VectorColumn& col = columns_[*c];
        if (col.json) return fail(dstd::fmt<"'{}' is a json field">(col.name));
        V v{dataframe::expr_col(static_cast<std::int32_t>(*c)),
            column_kind(col.type.id)};
        const dataframe::DataType* type = &col.type;
        v.elem = element(*type);
        for (std::size_t i = consumed; i < f.steps.size(); ++i) {
            if (v.k != K::List || !f.steps[i].index) return null();
            v.e = dataframe::expr_list_get(v.e, *f.steps[i].index);
            type = &type->fields.front().type;
            v.k = kind_of(type->id);
            v.elem = element(*type);
        }
        if (v.k == K::Other) return null();
        return v;
    }

    static K element(const dataframe::DataType& t) {
        if (kind_of(t.id) != K::List || t.fields.empty()) return K::Other;
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
        return {dataframe::expr_is_null(
                    dataframe::expr_col(static_cast<std::int32_t>(*c)), false),
                K::Bool};
    }

    V node(const TUnary& u) {
        const V v = (*this)(*u.operand);
        if (u.op == TermOp::Not)
            return v.k == K::Bool ? V{dataframe::expr_not(v.e), K::Bool}
                                  : null();
        if (!number_like(v.k)) return null();
        return {dataframe::expr_neg(v.e), v.k == K::Uint ? K::Int : v.k};
    }

    static K arith_kind(TermOp op, K a, K b) {
        if (op == TermOp::Div || a == K::Float || b == K::Float)
            return K::Float;
        return K::Int;
    }

    static dataframe::ArithOp arith_op(TermOp op) {
        switch (op) {
            case TermOp::Add:
                return dataframe::ArithOp::Add;
            case TermOp::Sub:
                return dataframe::ArithOp::Sub;
            case TermOp::Mul:
                return dataframe::ArithOp::Mul;
            case TermOp::Div:
                return dataframe::ArithOp::Div;
            case TermOp::Idiv:
                return dataframe::ArithOp::FloorDiv;
            default:
                return dataframe::ArithOp::Mod;
        }
    }

    static dstd::Optional<CmpOp> cmp_op(TermOp op) {
        switch (op) {
            case TermOp::Eq:
                return CmpOp::Eq;
            case TermOp::Ne:
                return CmpOp::Ne;
            case TermOp::Lt:
                return CmpOp::Lt;
            case TermOp::Le:
                return CmpOp::Le;
            case TermOp::Gt:
                return CmpOp::Gt;
            case TermOp::Ge:
                return CmpOp::Ge;
            default:
                return dstd::nullopt;
        }
    }

    static bool comparable(K a, K b) {
        if (number_like(a) && number_like(b)) return true;
        if ((a == K::Any && (b == K::Str || b == K::Bool)) ||
            (b == K::Any && (a == K::Str || a == K::Bool)))
            return true;
        return a == b && (a == K::Str || a == K::Bool);
    }

    // `a op b` over values of one domain; null across domains.
    static V compare(CmpOp op, const V& a, const V& b) {
        if (!comparable(a.k, b.k)) return null();
        if (b.constant && a.k == b.k && (a.k == K::Int || a.k == K::Float)) {
            const auto& c = b.constant->value;
            const dftu_scalar s =
                a.k == K::Float
                    ? dataframe::detail::expr_scalar_d(std::get<double>(c))
                    : dataframe::detail::expr_scalar_i(
                          std::holds_alternative<std::int64_t>(c)
                              ? std::get<std::int64_t>(c)
                              : static_cast<std::int64_t>(
                                    std::get<std::uint64_t>(c)));
            return {dataframe::expr_cmp(op, a.e, s), K::Bool};
        }
        return {dataframe::expr_cmp_expr(op, a.e, b.e), K::Bool};
    }

    static V both(LogicalOp op, const V& a, const V& b) {
        return {dataframe::expr_logical(op, as_bool(a), as_bool(b)), K::Bool};
    }

    // One kind for the operands of a value-choosing form, or a compile error.
    K common(const dstd::Vector<V>& vs, dstd::StringView what) const {
        K out = K::Nul;
        for (const V& v : vs) {
            if (v.k == K::Nul) continue;
            if (out == K::Nul || out == v.k) {
                out = v.k;
            } else if (out == K::Any || v.k == K::Any) {
                out = K::Any;
            } else if (numeric(out) && numeric(v.k)) {
                out = out == K::Float || v.k == K::Float ? K::Float : K::Int;
            } else {
                fail(dstd::fmt<
                     "{} chooses between values of different types; convert "
                     "them with int(), float() or string()">(what));
                return K::Nul;
            }
        }
        return out;
    }

    V node(const TBinary& b) {
        const V l = (*this)(*b.left);
        const V r = (*this)(*b.right);
        switch (b.op) {
            case TermOp::And:
                return both(LogicalOp::And, l, r);
            case TermOp::Or:
                return both(LogicalOp::Or, l, r);
            case TermOp::Coalesce:
                return coalesce({l, r}, "the ?? operator");
            default:
                break;
        }
        if (auto op = cmp_op(b.op)) return compare(*op, l, r);
        if (!number_like(l.k) || !number_like(r.k)) return null();
        return {dataframe::expr_arith(arith_op(b.op), l.e, r.e),
                l.k == K::Any || r.k == K::Any ? K::Any
                                               : arith_kind(b.op, l.k, r.k)};
    }

    V coalesce(dstd::Vector<V> vs, dstd::StringView what) const {
        const K k = common(vs, what);
        dstd::Vector<Expr> args;
        for (const V& v : vs)
            if (v.k != K::Nul) args.push_back(v.e);
        if (args.empty()) return null();
        if (args.size() == 1) return {args.front(), k};
        return {dataframe::expr_coalesce(args), k};
    }

    V node(const TBetween& b) {
        const V v = (*this)(*b.subject);
        const V t = both(LogicalOp::And, compare(CmpOp::Ge, v, (*this)(*b.low)),
                         compare(CmpOp::Le, v, (*this)(*b.high)));
        return b.negated ? V{dataframe::expr_not(t.e), K::Bool} : t;
    }

    V node(const TIs& is) {
        if (is.missing) {
            const V p = present(*is.subject);
            return is.negated ? p : V{dataframe::expr_not(p.e), K::Bool};
        }
        const V v = (*this)(*is.subject);
        if (v.k == K::Nul) return boolean(!is.negated);
        return {dataframe::expr_is_null(v.e, !is.negated), K::Bool};
    }

    V node(const TIn& in) {
        const V v = (*this)(*in.subject);
        dstd::Vector<V> items;
        for (const auto& item : in.list) {
            V x = (*this)(*item);
            if (x.k != K::Nul && comparable(v.k, x.k)) items.push_back(x);
        }
        if (items.empty()) return null();
        // YES if any item equals, NO if any compares, else unknown.
        Expr any_equal;
        Expr any_compared;
        for (const V& x : items) {
            const Expr eq = compare(CmpOp::Eq, v, x).e;
            const Expr hit =
                dataframe::expr_coalesce({eq, dataframe::expr_lit_bool(false)});
            const Expr known = dataframe::expr_is_null(eq, false);
            any_equal = any_equal.valid() ? dataframe::expr_logical(
                                                LogicalOp::Or, any_equal, hit)
                                          : hit;
            any_compared = any_compared.valid()
                               ? dataframe::expr_logical(LogicalOp::Or,
                                                         any_compared, known)
                               : known;
        }
        Expr t = dataframe::expr_select(
            any_equal, dataframe::expr_lit_bool(true),
            dataframe::expr_select(any_compared,
                                   dataframe::expr_lit_bool(false),
                                   dataframe::expr_lit_null(TypeId::Bool)));
        return {in.negated ? dataframe::expr_not(t) : t, K::Bool};
    }

    V node(const TIndex& x) {
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].index == &x) return column_value(i);
        return fail(
            "an array index runs on columns only through "
            "call_column()");
    }

    V node(const TList& l) {
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].list == &l) return column_value(i);
        return fail(
            "a list literal runs on columns only through "
            "call_column()");
    }

    V node(const TQuant& q) {
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].quant == &q)
                return {dataframe::expr_col(static_cast<std::int32_t>(i)),
                        K::Bool};
        return fail(
            dstd::fmt<"{}() runs on a list column only through quantify()">(
                q.all ? "all" : "any"));
    }

    V node(const TLookup& l) {
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].lookup == &l) {
                const K k = kind_of(columns_[i].type.id);
                if (k == K::Other || k == K::List) return null();
                return {dataframe::expr_col(static_cast<std::int32_t>(i)), k};
            }
        return fail(dstd::fmt<"a lookup into '{}' runs on columns only "
                              "through lookup()">(l.name));
    }

    // The column of `t` when it is a parse_json() call.
    const V* json_call(const Term& t) {
        const auto* c = std::get_if<TCall>(&t.node);
        if (!c || c->fn != Fn::ParseJson) return nullptr;
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].call == c) {
                json_ = {dataframe::expr_col(static_cast<std::int32_t>(i)),
                         K::Str};
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
            return fail(
                "parse_json() gives a json value, which only json() "
                "and a whole select or derive item read here");
        V v{dataframe::expr_col(static_cast<std::int32_t>(i)),
            kind_of(columns_[i].type.id)};
        v.elem = element(columns_[i].type);
        if (v.k == K::Other) return null();
        return v;
    }

    V column_call(const TCall& c) {
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].call == &c) return column_value(i);
        return fail(
            dstd::fmt<"{}() runs on columns only through call_column()">(
                fn_name(c)));
    }

    V node(const TMatch& m) {
        const V v = (*this)(*m.subject);
        if (v.k != K::Str || !m.compiled) return null();
        const Expr t = dataframe::expr_str_pattern(v.e, m.compiled);
        return {m.negated ? dataframe::expr_not(t) : t, K::Bool};
    }

    // contains() over a list literal: whether `p` equals an item, unknown
    // when `p` is.
    V contains_item(const TList& l, const V& p) {
        if (p.k == K::Nul) return null();
        Expr any;
        for (const auto& item : l.items) {
            const V x = (*this)(*item);
            if (x.k == K::Nul || !comparable(p.k, x.k)) continue;
            const Expr hit = dataframe::expr_coalesce(
                {compare(CmpOp::Eq, p, x).e, dataframe::expr_lit_bool(false)});
            any = any.valid() ? dataframe::expr_logical(LogicalOp::Or, any, hit)
                              : hit;
        }
        if (!any.valid()) any = dataframe::expr_lit_bool(false);
        if (p.constant) return {any, K::Bool};
        return {
            dataframe::expr_select(dataframe::expr_is_null(p.e, true),
                                   dataframe::expr_lit_null(TypeId::Bool), any),
            K::Bool};
    }

    // A function runs on operands of known types only; one the plan types
    // per batch reaches it through a per-batch compile.
    V arg(const TCall& c, std::size_t i) {
        V v = (*this)(*c.args[i]);
        if (v.k == K::Any)
            return fail(
                dstd::fmt<"{}() reads '{}', whose type only each "
                          "batch gives">(fn_name(c), term_text(*c.args[i])));
        return v;
    }

    static const dstd::String* const_str(const V& v) {
        return v.constant ? std::get_if<dstd::String>(&v.constant->value)
                          : nullptr;
    }

    static dstd::Optional<std::int64_t> const_int(const V& v) {
        if (!v.constant) return dstd::nullopt;
        if (const auto* i = std::get_if<std::int64_t>(&v.constant->value))
            return *i;
        if (const auto* u = std::get_if<std::uint64_t>(&v.constant->value))
            if (*u <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max()))
                return static_cast<std::int64_t>(*u);
        return dstd::nullopt;
    }

    static dstd::StringView fn_name(const TCall& c) {
        return fn_info(c.fn).name;
    }

    static V text(const V& v) {
        switch (v.k) {
            case K::Str:
                return v;
            case K::Int:
            case K::Uint:
            case K::Float:
            case K::Bool:
                return {
                    dataframe::expr_convert(dataframe::ConvertOp::String, v.e),
                    K::Str};
            default:
                return null();
        }
    }

    // `cond` as a Bool that is false where it is unknown.
    static Expr yes(const V& cond) {
        if (cond.k != K::Bool) return dataframe::expr_lit_bool(false);
        return dataframe::expr_coalesce(
            {cond.e, dataframe::expr_lit_bool(false)});
    }

    V choose(const V& cond, const V& a, const V& b) {
        const K k = common({a, b}, "if()");
        if (k == K::Nul) return null();
        return {dataframe::expr_select(yes(cond), as(a, k), as(b, k)), k};
    }

    V node(const TCall& c) {
        switch (c.fn) {
            case Fn::Exists:
                return present(*c.args[0]);
            case Fn::Coalesce: {
                dstd::Vector<V> vs;
                for (const auto& a : c.args) vs.push_back((*this)(*a));
                return coalesce(std::move(vs), "coalesce()");
            }
            case Fn::If:
                return choose(arg(c, 0), arg(c, 1), arg(c, 2));
            case Fn::Case: {
                V out = arg(c, c.args.size() - 1);
                for (std::size_t i = c.args.size() - 1; i >= 2; i -= 2)
                    out = choose(arg(c, i - 2), arg(c, i - 1), out);
                return out;
            }
            case Fn::Abs: {
                const V v = arg(c, 0);
                if (!numeric(v.k)) return null();
                if (v.k == K::Float)
                    return {dataframe::expr_unary(dataframe::UnaryOp::Abs, v.e),
                            K::Float};
                if (v.k == K::Uint) return v;
                return {dataframe::expr_select(
                            dataframe::expr_cmp(
                                CmpOp::Lt, v.e,
                                dataframe::detail::expr_scalar_i(0)),
                            dataframe::expr_neg(v.e), v.e),
                        K::Int};
            }
            case Fn::Floor:
            case Fn::Ceil: {
                const V v = arg(c, 0);
                if (!numeric(v.k)) return null();
                if (v.k != K::Float) return v;
                return {dataframe::expr_unary(c.fn == Fn::Floor
                                                  ? dataframe::UnaryOp::Floor
                                                  : dataframe::UnaryOp::Ceil,
                                              v.e),
                        K::Float};
            }
            case Fn::Round: {
                const V v = arg(c, 0);
                std::int64_t digits = 0;
                if (c.args.size() > 1) {
                    const V d = arg(c, 1);
                    if (!numeric(d.k)) return null();
                    if (!numeric(v.k)) return null();
                    const auto n = const_int(d);
                    if (!n)
                        return {dataframe::expr_round_col(v.e, d.e),
                                v.k == K::Float ? K::Float : K::Int};
                    digits = *n;
                }
                if (!numeric(v.k)) return null();
                return {dataframe::expr_round(v.e, digits),
                        v.k == K::Float ? K::Float : K::Int};
            }
            case Fn::Min:
            case Fn::Max: {
                if (c.args.size() == 1) {
                    if (arg(c, 0).k == K::List)
                        return no_kernel("min() and max() of a list");
                    return null();
                }
                dstd::Vector<Expr> args;
                K k = K::Nul;
                for (const auto& a : c.args) {
                    const V v = (*this)(*a);
                    if (v.k == K::Nul) return null();
                    const bool same = k == K::Nul || comparable(k, v.k);
                    if (!same || v.k == K::Bool || v.k == K::List ||
                        v.k == K::Other)
                        return null();
                    k = k == K::Nul || k == v.k ? v.k : K::Float;
                    args.push_back(v.e);
                }
                return {dataframe::expr_extreme(args, c.fn == Fn::Min), k};
            }
            case Fn::Log:
            case Fn::Exp: {
                const V v = arg(c, 0);
                if (!numeric(v.k)) return null();
                return {c.fn == Fn::Log ? dataframe::expr_log(v.e)
                                        : dataframe::expr_unary(
                                              dataframe::UnaryOp::Exp, v.e),
                        K::Float};
            }
            case Fn::Pow: {
                const V a = arg(c, 0);
                const V b = arg(c, 1);
                if (!numeric(a.k) || !numeric(b.k)) return null();
                return {dataframe::expr_pow(a.e, b.e), K::Float};
            }
            case Fn::Len: {
                const V v = arg(c, 0);
                if (v.k == K::Str)
                    return {dataframe::expr_str_size(v.e, true), K::Int};
                if (v.k == K::List)
                    return {dataframe::expr_list_size(v.e), K::Int};
                return null();
            }
            case Fn::Concat: {
                dstd::Vector<Expr> args;
                for (const auto& a : c.args) {
                    const V t = text((*this)(*a));
                    if (t.k == K::Nul) return null();
                    args.push_back(t.e);
                }
                if (args.size() == 1) return {args.front(), K::Str};
                return {dataframe::expr_concat(args), K::Str};
            }
            case Fn::Lower:
            case Fn::Upper:
            case Fn::Trim: {
                const V v = arg(c, 0);
                if (v.k != K::Str) return null();
                return {dataframe::expr_str_map(
                            c.fn == Fn::Lower   ? dataframe::StrMapOp::Lower
                            : c.fn == Fn::Upper ? dataframe::StrMapOp::Upper
                                                : dataframe::StrMapOp::Strip,
                            v.e),
                        K::Str};
            }
            case Fn::StartsWith:
            case Fn::EndsWith: {
                const V v = arg(c, 0);
                const V p = arg(c, 1);
                if (v.k != K::Str || p.k != K::Str) return null();
                const auto op = c.fn == Fn::StartsWith
                                    ? dataframe::StrPredOp::StartsWith
                                    : dataframe::StrPredOp::EndsWith;
                if (const auto* n = const_str(p))
                    return {dataframe::expr_str_pred(op, v.e, *n), K::Bool};
                return checked(dataframe::expr_str_pred_col(op, v.e, p.e),
                               K::Bool);
            }
            case Fn::Contains: {
                if (const auto* l = std::get_if<TList>(&c.args[0]->node))
                    return contains_item(*l, arg(c, 1));
                const V v = arg(c, 0);
                const V p = arg(c, 1);
                if (p.k == K::Nul) return null();
                if (v.k == K::Str) {
                    if (p.k != K::Str) return null();
                    if (const auto* n = const_str(p))
                        return {dataframe::expr_str_pred(
                                    dataframe::StrPredOp::Contains, v.e, *n),
                                K::Bool};
                    return checked(
                        dataframe::expr_str_pred_col(
                            dataframe::StrPredOp::Contains, v.e, p.e),
                        K::Bool);
                }
                if (v.k != K::List) return null();
                if (const auto i = const_int(p))
                    return {dataframe::expr_list_contains(
                                v.e, dataframe::detail::expr_scalar_i(*i)),
                            K::Bool};
                if (p.constant && p.k == K::Float)
                    return {dataframe::expr_list_contains(
                                v.e, dataframe::detail::expr_scalar_d(
                                         std::get<double>(p.constant->value))),
                            K::Bool};
                return no_kernel(
                    "contains() of a list with a non-numeric literal");
            }
            case Fn::Substr: {
                const V v = arg(c, 0);
                const V s = arg(c, 1);
                if (v.k != K::Str || s.k == K::Nul) return null();
                if (!numeric(s.k)) return null();
                dstd::Optional<V> l;
                if (c.args.size() > 2) {
                    l = arg(c, 2);
                    if (l->k == K::Nul || !numeric(l->k)) return null();
                }
                const auto start = const_int(s);
                const auto n = l ? const_int(*l) : dstd::nullopt;
                if (!start || (l && !n)) {
                    return {dataframe::expr_str_substr_col(v.e, s.e,
                                                           l ? &l->e : nullptr),
                            K::Str};
                }
                std::int64_t len = -1;
                if (l) {
                    if (*n < 0) return null();
                    len = *n;
                }
                if (*start < 0) return null();
                return {dataframe::expr_str_substr(v.e, *start, len), K::Str};
            }
            case Fn::Replace: {
                const V v = arg(c, 0);
                const V from = arg(c, 1);
                const V to = arg(c, 2);
                if (v.k != K::Str || from.k != K::Str || to.k != K::Str)
                    return null();
                const dstd::String* f = const_str(from);
                const dstd::String* t = const_str(to);
                if (!f || !t)
                    return {dataframe::expr_str_replace_col(v.e, from.e, to.e),
                            K::Str};
                if (f->empty()) return v;
                return {dataframe::expr_str_replace(v.e, *f, *t, true), K::Str};
            }
            case Fn::First:
            case Fn::Last: {
                const V v = arg(c, 0);
                if (v.k != K::List || v.elem == K::Other) return null();
                return {
                    dataframe::expr_list_get(v.e, c.fn == Fn::First ? 0 : -1),
                    v.elem};
            }
            case Fn::Sum: {
                const V v = arg(c, 0);
                if (v.k != K::List || !numeric(v.elem)) return null();
                return {dataframe::expr_list_sum(v.e),
                        v.elem == K::Float ? K::Float : K::Int};
            }
            case Fn::Json: {
                if (const auto* j = json_call(*c.args[0])) return *j;
                if (const auto* f = std::get_if<TField>(&c.args[0]->node)) {
                    std::size_t consumed = 0;
                    const auto i = field_column(*f, consumed);
                    if (i && consumed == f->steps.size() && columns_[*i].json)
                        return {
                            dataframe::expr_col(static_cast<std::int32_t>(*i)),
                            K::Str};
                }
                const V v = arg(c, 0);
                if (v.k == K::Nul)
                    return std::holds_alternative<TField>(c.args[0]->node)
                               ? null()
                               : V{dataframe::expr_lit_str("null"), K::Str};
                if (v.k == K::List || v.k == K::Other)
                    return no_kernel("json() of a list");
                return {
                    dataframe::expr_convert(dataframe::ConvertOp::Json, v.e),
                    K::Str};
            }
            case Fn::Type: {
                const V v = arg(c, 0);
                dstd::StringView name;
                switch (v.k) {
                    case K::Int:
                    case K::Uint:
                    case K::Float:
                        name = "number";
                        break;
                    case K::Str:
                        name = "string";
                        break;
                    case K::Bool:
                        name = "bool";
                        break;
                    case K::List:
                        name = "array";
                        break;
                    default:
                        return {dataframe::expr_lit_str("null"), K::Str};
                }
                return {dataframe::expr_select(dataframe::expr_is_null(v.e),
                                               dataframe::expr_lit_str("null"),
                                               dataframe::expr_lit_str(name)),
                        K::Str};
            }
            case Fn::Int:
            case Fn::Float: {
                const V v = arg(c, 0);
                if (v.k == K::Nul || v.k == K::List || v.k == K::Other)
                    return null();
                const bool i = c.fn == Fn::Int;
                if (i && v.k == K::Int) return v;
                if (!i && v.k == K::Float) return v;
                return {dataframe::expr_convert(i ? dataframe::ConvertOp::Int
                                                  : dataframe::ConvertOp::Float,
                                                v.e),
                        i ? K::Int : K::Float};
            }
            case Fn::String:
                return text(arg(c, 0));
            case Fn::Slice:
            case Fn::Flatten:
            case Fn::Keys:
            case Fn::Values:
            case Fn::Split:
            case Fn::ParseJson:
            case Fn::IndexOf:
            case Fn::Sort:
            case Fn::Unique:
            case Fn::Join:
                return column_call(c);
            case Fn::Extract: {
                const V v = arg(c, 0);
                if (v.k != K::Str || !c.pattern) return null();
                std::int64_t group = 0;
                if (c.args.size() > 2) {
                    const V g = arg(c, 2);
                    if (g.k == K::Nul) return null();
                    if (!numeric(g.k)) return null();
                    const auto n = const_int(g);
                    if (!n)
                        return {dataframe::expr_str_extract_col(v.e, c.pattern,
                                                                g.e),
                                K::Str};
                    if (*n < 0) return null();
                    group = *n;
                }
                return {dataframe::expr_str_extract(v.e, c.pattern, group),
                        K::Str};
            }
            case Fn::RegexReplace: {
                const V v = arg(c, 0);
                if (v.k != K::Str || !c.pattern || !c.substitution)
                    return null();
                return {dataframe::expr_str_regex_replace(v.e, c.pattern,
                                                          *c.substitution),
                        K::Str};
            }
            case Fn::DatePart: {
                const V v = arg(c, 0);
                if (!numeric(v.k)) return null();
                return checked(
                    dataframe::expr_date_part(v.e, c.part, c.ns_per_unit),
                    K::Int);
            }
            case Fn::FormatTime: {
                const V v = arg(c, 0);
                if (!numeric(v.k)) return null();
                const auto* fmt = std::get_if<TConst>(&c.args[1]->node);
                return checked(
                    dataframe::expr_format_time(
                        v.e, std::get<dstd::String>(fmt->value), c.ns_per_unit),
                    K::Str);
            }
        }
        return null();
    }
};

}  // namespace

dstd::Result<dataframe::Expr> vectorize(
    const Term& t, const dstd::Vector<VectorColumn>& columns,
    bool args_fallback) {
    Vectorizer vz(columns, args_fallback);
    V v = vz.whole(t);
    if (vz.failed()) return unsupported(vz.error());
    if (v.k == K::Nul) return dataframe::expr_lit_null(TypeId::Bool);
    return std::move(v.e);
}

bool is_null_term(const Term& t, const dstd::Vector<VectorColumn>& columns,
                  bool args_fallback) {
    Vectorizer vz(columns, args_fallback);
    const V v = vz.whole(t);
    return !vz.failed() && v.k == K::Nul;
}

namespace {

// The key columns of `l` for `rows` rows: each key term evaluated, FLAT.
dstd::Result<dstd::Vector<dataframe::Series>> key_columns(
    const TLookup& l, const dstd::Vector<const dataframe::Series*>& inputs,
    const dstd::Vector<VectorColumn>& columns, std::int64_t rows,
    bool args_fallback) {
    dstd::Vector<dataframe::Series> out;
    for (const auto& k : l.keys) {
        Vectorizer vz(columns, args_fallback);
        const V v = vz(*k);
        if (vz.failed()) return unsupported(vz.error());
        if (v.k == K::Nul || v.k == K::Other || v.k == K::List) {
            out.push_back(dataframe::Series::nulls(TypeId::Bool, rows));
            continue;
        }
        dstd::Result<dataframe::Series> key = dataframe::eval(v.e, inputs);
        if (!key) return key.error();
        out.push_back(key->materialize());
    }
    return out;
}

// The table of `l`, or the error when its rows were not bound.
dstd::Result<const LookupTable*> bound(const TLookup& l) {
    if (!l.slot || !l.slot->table)
        return unsupported(
            dstd::fmt<"'{}' was read before its rows were bound">(l.name));
    return l.slot->table.get();
}

}  // namespace

dstd::Result<dataframe::Series> lookup(
    const TLookup& l, const dstd::Vector<const dataframe::Series*>& inputs,
    const dstd::Vector<VectorColumn>& columns, bool args_fallback) {
    const std::int64_t rows = inputs.empty() ? 0 : inputs.front()->length();
    if (auto table = bound(l); !table) return table.error();
    const dstd::Result<dstd::Vector<dataframe::Series>> keys =
        key_columns(l, inputs, columns, rows, args_fallback);
    if (!keys) return keys.error();
    dstd::Vector<const dataframe::Series*> ptrs;
    for (const auto& k : *keys) ptrs.push_back(&k);
    return lookup_column(l, ptrs, rows);
}

dstd::Result<dataframe::Series> quantify(
    const TQuant& q, const dstd::Vector<const dataframe::Series*>& inputs,
    const dstd::Vector<VectorColumn>& columns, bool args_fallback) {
    const std::int64_t rows = inputs.empty() ? 0 : inputs.front()->length();
    const dataframe::Series unknown =
        dataframe::Series::nulls(TypeId::Bool, rows);
    // Each row's elements are values[starts[r], starts[r + 1]).
    dataframe::Series values;
    dstd::Vector<std::int64_t> starts;
    dstd::Vector<std::uint8_t> present(static_cast<std::size_t>(rows), 0);
    if (const auto* l = std::get_if<TLookup>(&q.subject->node); l && l->all) {
        const dstd::Result<const LookupTable*> bound_table = bound(*l);
        if (!bound_table) return bound_table.error();
        const LookupTable& t = **bound_table;
        const dstd::Result<dstd::Vector<dataframe::Series>> keys =
            key_columns(*l, inputs, columns, rows, args_fallback);
        if (!keys) return keys.error();
        dstd::Vector<const dataframe::Series*> ptrs;
        for (const auto& k : *keys) ptrs.push_back(&k);
        Matches m = match(t, ptrs, rows);
        const auto n = static_cast<std::int64_t>(m.rows.size());
        values = t.value ? t.frame->columns[*t.value].take(m.rows)
                         : dataframe::Series::nulls(TypeId::String, n);
        starts = std::move(m.offsets);
        for (std::int64_t r = 0; r < rows; ++r)
            present[static_cast<std::size_t>(r)] =
                starts[static_cast<std::size_t>(r) + 1] >
                starts[static_cast<std::size_t>(r)];
    } else {
        Vectorizer subject_vz(columns, args_fallback);
        const V subject = subject_vz(*q.subject);
        if (subject_vz.failed()) return unsupported(subject_vz.error());
        if (subject.k != K::List) return unknown.share();
        dstd::Result<dataframe::Series> evaluated =
            dataframe::eval(subject.e, inputs);
        if (!evaluated) return evaluated.error();
        dataframe::Series list = evaluated->materialize();
        const std::int32_t* off = list.offsets();
        if (!off) return unknown.share();
        const dataframe::Series child = list.child(0);
        const auto n = static_cast<std::int64_t>(off[rows] - off[0]);
        if (off[0] == 0 && child.length() == n) {
            values = child.share();
        } else {
            dstd::Vector<std::int64_t> at(static_cast<std::size_t>(n));
            std::iota(at.begin(), at.end(), std::int64_t{off[0]});
            values = child.take(at);
        }
        for (std::int64_t r = 0; r <= rows; ++r)
            starts.push_back(off[r] - off[0]);
        for (std::int64_t r = 0; r < rows; ++r)
            present[static_cast<std::size_t>(r)] = !list.is_null(r);
    }
    dstd::Vector<std::int64_t> parent;
    parent.reserve(static_cast<std::size_t>(starts.back()));
    for (std::int64_t r = 0; r < rows; ++r)
        for (std::int64_t e = starts[static_cast<std::size_t>(r)];
             e < starts[static_cast<std::size_t>(r) + 1]; ++e)
            parent.push_back(r);
    const auto n = static_cast<std::int64_t>(parent.size());

    dstd::Vector<dataframe::Series> owned;
    dstd::Vector<VectorColumn> cols;
    owned.push_back(std::move(values));
    cols.push_back({".", owned.back().data_type()});
    if (owned.front().type() == TypeId::Struct)
        for (std::int64_t i = 0; i < owned.front().num_children(); ++i) {
            dataframe::Series field = owned.front().child(i);
            cols.push_back(
                {dstd::String(".").append(owned.front().field_name(i)),
                 field.data_type()});
            owned.push_back(std::move(field));
        }
    dstd::Vector<dstd::String> read;
    for_each_term_field(*q.cond,
                        [&](const TField& f) { read.push_back(f.base); });
    auto reads = [&](const VectorColumn& c) {
        if (c.quant || c.lookup || c.call) return true;
        for (const auto& b : read)
            if (b == dstd::StringView(c.name) ||
                (args_fallback && c.name.starts_with("args.") &&
                 dstd::StringView(c.name).substr(5) == b) ||
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
    dstd::Vector<const dataframe::Series*> frame;
    for (const auto& c : owned) frame.push_back(&c);
    Vectorizer cond_vz(cols, args_fallback);
    const V cond = cond_vz(*q.cond);
    if (cond_vz.failed()) return unsupported(cond_vz.error());
    dstd::Vector<std::uint8_t> yes(static_cast<std::size_t>(n), 0);
    dstd::Vector<std::uint8_t> known(static_cast<std::size_t>(n), 0);
    if (cond.k == K::Bool && n > 0) {
        dstd::Result<dataframe::Series> truth = dataframe::eval(cond.e, frame);
        if (!truth) return truth.error();
        const dataframe::Series t = truth->materialize();
        const auto* bits = t.data<std::uint8_t>();
        for (std::int64_t e = 0; e < n; ++e) {
            if (t.is_null(e)) continue;
            known[static_cast<std::size_t>(e)] = 1;
            yes[static_cast<std::size_t>(e)] = (bits[e >> 3] >> (e & 7)) & 1;
        }
    }
    dstd::Vector<std::uint8_t> out((static_cast<std::size_t>(rows) + 7) / 8, 0);
    dstd::Vector<std::uint8_t> valid((static_cast<std::size_t>(rows) + 7) / 8,
                                     0);
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
    return dataframe::Series::flat(TypeId::Bool, out.data(), rows,
                                   valid.data());
}

dstd::Result<dataframe::Expr> vectorize_condition(
    const Term& t, const dstd::Vector<VectorColumn>& columns,
    bool args_fallback) {
    Vectorizer vz(columns, args_fallback);
    V v = vz(t);
    if (vz.failed()) return unsupported(vz.error());
    if (v.k != K::Bool) return dataframe::expr_lit_null(TypeId::Bool);
    return std::move(v.e);
}

namespace {

bool is_list_type(const dataframe::DataType& t) {
    return (t.id == TypeId::List || t.id == TypeId::LargeList) &&
           !t.fields.empty();
}

// A LIST column's offsets and elements.
struct ListParts {
    dataframe::Series list;
    dataframe::Series child;
    dstd::Vector<std::int64_t> off;
};

dstd::Optional<ListParts> list_parts(const dataframe::Series& s) {
    ListParts p{s.is_flat() ? s.share() : s.materialize(), {}, {}};
    const std::int64_t n = p.list.length();
    if (const auto* o = p.list.offsets()) {
        p.off.assign(o, o + n + 1);
    } else if (const auto* o64 = p.list.offsets64()) {
        p.off.assign(o64, o64 + n + 1);
    } else {
        return dstd::nullopt;
    }
    p.child = p.list.child(0);
    return p;
}

// A LIST column of `rows[offsets[r], offsets[r + 1])` of `values`, null
// where `valid` is false.
dataframe::Series make_list(const dstd::Vector<std::int64_t>& offsets,
                            dataframe::Series values,
                            const dstd::Vector<bool>& valid) {
    dstd::Vector<std::int32_t> off(offsets.begin(), offsets.end());
    dataframe::Series out = dataframe::Series::list(off, std::move(values));
    if (std::all_of(valid.begin(), valid.end(), [](bool v) { return v; }))
        return out;
    dstd::Vector<std::int64_t> rows(valid.size());
    for (std::size_t r = 0; r < valid.size(); ++r)
        rows[r] = valid[r] ? static_cast<std::int64_t>(r) : -1;
    return out.take(rows);
}

dataframe::Series flat_of(const dataframe::Series& s) {
    return s.is_flat() ? s.share() : s.materialize();
}

dstd::Optional<std::int64_t> int_at(const dataframe::Series& s,
                                    std::int64_t r) {
    if (s.is_null(r)) return dstd::nullopt;
    return s.data<std::int64_t>()[r];
}

std::int64_t clamp_at(std::int64_t i, std::int64_t size) {
    if (i < 0) return std::max<std::int64_t>(0, size + i);
    return std::min(i, size);
}

dataframe::Series slice_column(const dstd::Vector<dataframe::Series>& args) {
    auto p = list_parts(args[0]);
    const std::int64_t n = args[0].length();
    const dataframe::Series from = args[1].cast(TypeId::Int64).materialize();
    const dataframe::Series to =
        args.size() > 2 ? args[2].cast(TypeId::Int64).materialize()
                        : dataframe::Series::nulls(TypeId::Int64, n);
    dstd::Vector<std::int64_t> offsets{0};
    dstd::Vector<std::int64_t> take;
    dstd::Vector<bool> valid(static_cast<std::size_t>(n), false);
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

dataframe::Series flatten_column(const dataframe::Series& arg) {
    auto p = list_parts(arg);
    const std::int64_t n = arg.length();
    auto inner = list_parts(p->child);
    if (!inner) return p->list.share();
    dstd::Vector<std::int64_t> offsets{0};
    dstd::Vector<std::int64_t> take;
    dstd::Vector<bool> valid(static_cast<std::size_t>(n), false);
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
dstd::Vector<dstd::Pair<dstd::String, dataframe::Series>> object_fields(
    const dstd::Vector<dataframe::Series>& args,
    const dstd::Vector<dstd::String>& keys) {
    dstd::Vector<dstd::Pair<dstd::String, dataframe::Series>> out;
    if (args.size() == 1 && args[0].type() == TypeId::Struct) {
        const dataframe::Series st = flat_of(args[0]);
        for (std::int64_t i = 0; i < st.num_children(); ++i)
            out.emplace_back(st.field_name(i), flat_of(st.child(i)));
    } else {
        for (std::size_t i = 0; i < args.size(); ++i)
            out.emplace_back(keys[i], flat_of(args[i]));
    }
    dstd::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    return out;
}

// The one column of the single-column frames `ptrs`, joined. Every part has the
// element type call_type admitted, which concat always joins, so a failure
// here is a broken contract.
dataframe::Series joined_values(
    const dstd::Vector<const dataframe::DataFrame*>& ptrs) {
    dstd::Result<dataframe::DataFrame> all = dataframe::concat(ptrs);
    if (!all) dstd::fatal("vectorize: the parts of a list do not join");
    return std::move(all->columns.front());
}

dataframe::Series members_column(const dstd::Vector<dataframe::Series>& args,
                                 const dstd::Vector<dstd::String>& keys,
                                 bool names, const dataframe::DataType& type) {
    const std::int64_t n = args.empty() ? 0 : args[0].length();
    const auto fields = object_fields(args, keys);
    const bool whole = args.size() == 1 && args[0].type() == TypeId::Struct;
    dstd::Vector<std::int64_t> offsets{0};
    dstd::Vector<std::int64_t> take;
    dstd::Vector<bool> valid(static_cast<std::size_t>(n), false);
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
        dstd::Vector<dstd::StringView> all;
        for (const auto& f : fields) all.push_back(f.first);
        return make_list(
            offsets,
            dataframe::Series::strings(dstd::Span<const dstd::StringView>(all))
                .take(take),
            valid);
    }
    const TypeId elem = type.fields.front().type.id;
    dstd::Vector<dataframe::DataFrame> frames(fields.size());
    dstd::Vector<const dataframe::DataFrame*> ptrs;
    for (std::size_t k = 0; k < fields.size(); ++k) {
        frames[k].names.push_back("v");
        frames[k].columns.push_back(fields[k].second.type() == elem
                                        ? fields[k].second.share()
                                        : fields[k].second.cast(elem));
        ptrs.push_back(&frames[k]);
    }
    dataframe::Series values = joined_values(ptrs).take(take);
    return make_list(offsets, std::move(values), valid);
}

dataframe::Series parse_column(const dataframe::Series& arg) {
    const dataframe::Series s = flat_of(arg);
    const std::int64_t n = s.length();
    dftracer::utils::json::RecordParser parser;
    dstd::Vector<dstd::String> text(static_cast<std::size_t>(n));
    dstd::Vector<std::uint8_t> bits(static_cast<std::size_t>((n + 7) / 8), 0);
    for (std::int64_t r = 0; r < n; ++r) {
        if (s.is_null(r)) continue;
        const dstd::StringView in = s.string_at(r);
        simdjson::dom::element el;
        if (parser.parse(in.data(), in.size()).get(el) != simdjson::SUCCESS)
            continue;
        json::append_canonical_json(text[static_cast<std::size_t>(r)], el);
        bits[static_cast<std::size_t>(r) >> 3] |=
            static_cast<std::uint8_t>(1u << (r & 7));
    }
    const dstd::Vector<dstd::StringView> views(text.begin(), text.end());
    return dataframe::Series::strings(views, bits.data());
}

dataframe::Series split_column(const dataframe::Series& arg,
                               const dataframe::Series& sep_arg) {
    const dataframe::Series s = flat_of(arg);
    const dataframe::Series sep = flat_of(sep_arg);
    const std::int64_t n = s.length();
    dstd::Vector<std::int64_t> offsets{0};
    dstd::Vector<dstd::StringView> parts;
    dstd::Vector<bool> valid(static_cast<std::size_t>(n), false);
    for (std::int64_t r = 0; r < n; ++r) {
        if (!s.is_null(r) && !sep.is_null(r) && !sep.string_at(r).empty()) {
            const dstd::StringView text = s.string_at(r);
            const dstd::StringView p = sep.string_at(r);
            std::size_t at = 0;
            for (;;) {
                const std::size_t hit = text.find(p, at);
                parts.push_back(text.substr(at, hit - at));
                if (hit == dstd::StringView::npos) break;
                at = hit + p.size();
            }
            valid[static_cast<std::size_t>(r)] = true;
        }
        offsets.push_back(static_cast<std::int64_t>(parts.size()));
    }
    return make_list(offsets, dataframe::Series::strings(parts), valid);
}

using Cell = dstd::Variant<Number, bool, dstd::StringView>;

dstd::Optional<int> cmp_cell(const Cell& a, const Cell& b) {
    if (a.index() != b.index()) return dstd::nullopt;
    if (const auto* x = std::get_if<Number>(&a))
        return compare_numbers(*x, std::get<Number>(b));
    if (const auto* x = std::get_if<bool>(&a))
        return static_cast<int>(*x) - static_cast<int>(std::get<bool>(b));
    const int c =
        std::get<dstd::StringView>(a).compare(std::get<dstd::StringView>(b));
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

bool scalar_kind(K k) { return numeric(k) || k == K::Str || k == K::Bool; }

// The cells of a column of one scalar kind, read without converting a row.
class Cells {
   public:
    explicit Cells(const dataframe::Series& s)
        : k_(kind_of(s.type())), s_(prepare(s, k_)) {}

    bool null(std::int64_t i) const { return s_.is_null(i); }

    Cell at(std::int64_t i) const {
        switch (k_) {
            case K::Int:
                return Number{s_.data<std::int64_t>()[i]};
            case K::Uint:
                return Number{s_.data<std::uint64_t>()[i]};
            case K::Float:
                return Number{s_.data<double>()[i]};
            case K::Bool:
                return ((s_.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1) != 0;
            default:
                return s_.string_at(i);
        }
    }

   private:
    K k_;
    dataframe::Series s_;

    static dataframe::Series prepare(const dataframe::Series& s, K k) {
        if (k == K::Int) return flat_of(s.cast(TypeId::Int64));
        if (k == K::Float) return flat_of(s.cast(TypeId::Float64));
        return flat_of(s);
    }
};

// The element at each row's index, 0-based and from the end when negative.
dataframe::Series index_column(const dstd::Vector<dataframe::Series>& args) {
    auto p = list_parts(args[0]);
    const std::int64_t n = args[0].length();
    const Cells at(args[1]);
    dstd::Vector<std::int64_t> take(static_cast<std::size_t>(n), -1);
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

dataframe::Series index_of_column(const dataframe::Series& arg,
                                  const dataframe::Series& value) {
    auto p = list_parts(arg);
    const std::int64_t n = arg.length();
    const Cells elems(p->child);
    const Cells v(value);
    dstd::Vector<std::int64_t> out(static_cast<std::size_t>(n), 0);
    dstd::Vector<std::uint8_t> bits(static_cast<std::size_t>((n + 7) / 8), 0);
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
    return dataframe::Series::flat(TypeId::Int64, out.data(), n, bits.data());
}

// Each row's elements ascending, nulls last; null where a present element
// does not compare with the first.
dataframe::Series sort_column(const dataframe::Series& arg) {
    auto p = list_parts(arg);
    const std::int64_t n = arg.length();
    const Cells cells(p->child);
    dstd::Vector<std::int64_t> offsets{0};
    dstd::Vector<std::int64_t> take;
    dstd::Vector<std::int64_t> present;
    dstd::Vector<bool> valid(static_cast<std::size_t>(n), false);
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
                dstd::stable_sort(present.begin(), present.end(),
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
dataframe::Series unique_column(const dataframe::Series& arg) {
    auto p = list_parts(arg);
    const std::int64_t n = arg.length();
    const Cells cells(p->child);
    auto same = [&](std::int64_t a, std::int64_t b) {
        if (cells.null(a) || cells.null(b))
            return cells.null(a) && cells.null(b);
        const auto o = cmp_cell(cells.at(a), cells.at(b));
        return o && *o == 0;
    };
    dstd::Vector<std::int64_t> offsets{0};
    dstd::Vector<std::int64_t> take;
    dstd::Vector<bool> valid(static_cast<std::size_t>(n), false);
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
dataframe::Series join_column(const dataframe::Series& arg,
                              const dataframe::Series& sep_arg) {
    auto p = list_parts(arg);
    const std::int64_t n = arg.length();
    const dataframe::Series elems = flat_of(p->child);
    const bool strings = kind_of(elems.type()) == K::Str;
    const dataframe::Series sep = flat_of(sep_arg);
    dstd::String buf;
    dstd::Vector<dstd::Pair<std::size_t, std::size_t>> spans(
        static_cast<std::size_t>(n));
    dstd::Vector<std::uint8_t> bits(static_cast<std::size_t>((n + 7) / 8), 0);
    for (std::int64_t r = 0; r < n; ++r) {
        if (p->list.is_null(r) || sep.is_null(r)) continue;
        const std::size_t begin = buf.size();
        const dstd::StringView s = sep.string_at(r);
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
    dstd::Vector<dstd::StringView> views;
    views.reserve(spans.size());
    for (const auto& [b, e] : spans) views.emplace_back(buf.data() + b, e - b);
    return dataframe::Series::strings(views, bits.data());
}

// One list per row of the items' values at that row.
dataframe::Series list_column(const dstd::Vector<dataframe::Series>& items,
                              std::int64_t n, const dataframe::DataType& type) {
    const TypeId elem = type.fields.front().type.id;
    const auto m = static_cast<std::int64_t>(items.size());
    dstd::Vector<std::int64_t> offsets(static_cast<std::size_t>(n) + 1, 0);
    for (std::int64_t r = 0; r <= n; ++r)
        offsets[static_cast<std::size_t>(r)] = r * m;
    const dstd::Vector<bool> valid(static_cast<std::size_t>(n), true);
    if (items.empty())
        return make_list(offsets, dataframe::Series::nulls(elem, 0), valid);
    dstd::Vector<dataframe::DataFrame> frames(items.size());
    dstd::Vector<const dataframe::DataFrame*> ptrs;
    for (std::size_t k = 0; k < items.size(); ++k) {
        const dataframe::Series s = items[k].null_count() == n
                                        ? dataframe::Series::nulls(elem, n)
                                        : flat_of(items[k]);
        frames[k].names.push_back("v");
        frames[k].columns.push_back(s.type() == elem ? s.share()
                                                     : s.cast(elem));
        ptrs.push_back(&frames[k]);
    }
    dstd::Vector<std::int64_t> take;
    take.reserve(static_cast<std::size_t>(n * m));
    for (std::int64_t r = 0; r < n; ++r)
        for (std::int64_t k = 0; k < m; ++k) take.push_back(k * n + r);
    return make_list(offsets, joined_values(ptrs).take(take), valid);
}

}  // namespace

bool is_column_call(Fn fn) {
    return fn == Fn::Slice || fn == Fn::Flatten || fn == Fn::Keys ||
           fn == Fn::Values || fn == Fn::ParseJson || fn == Fn::Split ||
           fn == Fn::IndexOf || fn == Fn::Sort || fn == Fn::Unique ||
           fn == Fn::Join;
}

bool is_column_term(const Term& t) {
    if (const auto* c = std::get_if<TCall>(&t.node))
        return is_column_call(c->fn);
    return std::holds_alternative<TIndex>(t.node) ||
           std::holds_alternative<TList>(t.node);
}

namespace {

// call_type() with the error in `error` (set only when it returns nullopt).
dstd::Optional<dataframe::DataType> call_type_of(
    const Term& t, const dstd::Vector<dataframe::DataType>& args,
    dstd::String& error) {
    auto integer = [](const dataframe::DataType& d) {
        return kind_of(d.id) == K::Int || kind_of(d.id) == K::Uint;
    };
    if (std::holds_alternative<TIndex>(t.node)) {
        if (!is_list_type(args[0]) || !integer(args[1])) return dstd::nullopt;
        return args[0].fields.front().type;
    }
    if (std::holds_alternative<TList>(t.node)) {
        if (args.empty())
            return dataframe::list_of(dataframe::scalar(TypeId::Int64));
        K k = K::Nul;
        for (const auto& a : args) {
            if (a.id == TypeId::Unknown) continue;
            const K x = kind_of(a.id);
            if (!scalar_kind(x) ||
                (k != K::Nul && k != x && !(numeric(k) && numeric(x)))) {
                error = "a list after the scan needs items of one type";
                return dstd::nullopt;
            }
            k = k == K::Nul || k == x ? x : K::Float;
        }
        switch (k) {
            case K::Int:
                return dataframe::list_of(dataframe::scalar(TypeId::Int64));
            case K::Uint:
                return dataframe::list_of(dataframe::scalar(TypeId::Uint64));
            case K::Float:
                return dataframe::list_of(dataframe::scalar(TypeId::Float64));
            case K::Str:
                return dataframe::list_of(dataframe::scalar(TypeId::String));
            default:
                return dataframe::list_of(dataframe::scalar(TypeId::Bool));
        }
    }
    const TCall& c = std::get<TCall>(t.node);
    switch (c.fn) {
        case Fn::Slice:
            if (!is_list_type(args[0])) return dstd::nullopt;
            for (std::size_t i = 1; i < args.size(); ++i)
                if (!integer(args[i])) return dstd::nullopt;
            return dataframe::list_of(args[0].fields.front().type);
        case Fn::Flatten: {
            if (!is_list_type(args[0])) return dstd::nullopt;
            const dataframe::DataType& elem = args[0].fields.front().type;
            return dataframe::list_of(
                is_list_type(elem) ? elem.fields.front().type : elem);
        }
        case Fn::Keys:
            if (args.empty()) return dstd::nullopt;
            return dataframe::list_of(dataframe::scalar(TypeId::String));
        case Fn::Values: {
            dstd::Vector<dataframe::DataType> fields = args;
            if (args.size() == 1 && args[0].id == TypeId::Struct) {
                fields.clear();
                for (const auto& f : args[0].fields) fields.push_back(f.type);
            }
            if (fields.empty()) return dstd::nullopt;
            K k = kind_of(fields.front().id);
            for (const auto& f : fields) {
                const K x = kind_of(f.id);
                if (x == k) continue;
                if (numeric(x) && numeric(k)) {
                    k = K::Float;
                    continue;
                }
                error =
                    "values() reads fields of different types; convert them "
                    "with int(), float() or string()";
                return dstd::nullopt;
            }
            if (k == K::Other || k == K::List) {
                error = "values() reads a field that holds a list or object";
                return dstd::nullopt;
            }
            return dataframe::list_of(dataframe::scalar(type_of(k)));
        }
        case Fn::ParseJson:
            if (kind_of(args[0].id) != K::Str) return dstd::nullopt;
            return dataframe::scalar(TypeId::String);
        case Fn::Split:
            if (kind_of(args[0].id) != K::Str || kind_of(args[1].id) != K::Str)
                return dstd::nullopt;
            return dataframe::list_of(dataframe::scalar(TypeId::String));
        case Fn::IndexOf:
            if (!is_list_type(args[0]) ||
                !scalar_kind(kind_of(args[0].fields.front().type.id)) ||
                !scalar_kind(kind_of(args[1].id)))
                return dstd::nullopt;
            return dataframe::scalar(TypeId::Int64);
        case Fn::Sort:
        case Fn::Unique:
            if (!is_list_type(args[0])) return dstd::nullopt;
            if (!scalar_kind(kind_of(args[0].fields.front().type.id))) {
                error = dstd::fmt<"{}() reads an array of arrays or objects">(
                    fn_info(c.fn).name);
                return dstd::nullopt;
            }
            return dataframe::list_of(args[0].fields.front().type);
        case Fn::Join:
            if (!is_list_type(args[0]) || kind_of(args[1].id) != K::Str)
                return dstd::nullopt;
            return dataframe::scalar(TypeId::String);
        default:
            return dstd::nullopt;
    }
}

}  // namespace

dstd::Result<dstd::Optional<dataframe::DataType>> call_type(
    const Term& t, const dstd::Vector<dataframe::DataType>& args) {
    dstd::String error;
    dstd::Optional<dataframe::DataType> type = call_type_of(t, args, error);
    if (!type && !error.empty())
        return common::make_error(common::ErrorCode::InvalidArgument, error);
    return type;
}

dataframe::Series call_column(const Term& t,
                              const dstd::Vector<dataframe::Series>& args,
                              const dstd::Vector<dstd::String>& keys,
                              const dataframe::DataType& type,
                              std::int64_t rows) {
    if (std::holds_alternative<TIndex>(t.node)) return index_column(args);
    if (std::holds_alternative<TList>(t.node))
        return list_column(args, rows, type);
    switch (std::get<TCall>(t.node).fn) {
        case Fn::Slice:
            return slice_column(args);
        case Fn::Flatten:
            return flatten_column(args[0]);
        case Fn::Keys:
        case Fn::Values:
            return members_column(args, keys,
                                  std::get<TCall>(t.node).fn == Fn::Keys, type);
        case Fn::ParseJson:
            return parse_column(args[0]);
        case Fn::Split:
            return split_column(args[0], args[1]);
        case Fn::IndexOf:
            return index_of_column(args[0], args[1]);
        case Fn::Sort:
            return sort_column(args[0]);
        case Fn::Unique:
            return unique_column(args[0]);
        case Fn::Join:
            return join_column(args[0], args[1]);
        default:
            return dataframe::Series::nulls(type.id, rows);
    }
}

}  // namespace dftracer::utils::duql
