// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_BUILDER_H
#define DFTRACER_UTILS_DUQL_BUILDER_H

#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/detail/ast.h>
#include <dftracer/utils/duql/detail/query_access.h>
#include <dftracer/utils/duql/detail/string_literal.h>
#include <dftracer/utils/duql/error.h>
#include <dftracer/utils/duql/types.h>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

// Render-only: Field/F/Expr and Expr::to_string() are header-only, so a plugin
// links nothing. Expr::build() has a deduced return type and is defined in
// query.h, so only consumers that include query.h can call it.
namespace dftracer::utils::duql {

namespace detail {

/// Turn a scalar into a @ref LiteralNode, mirroring the parser's typing rules:
/// non-negative integers become uint64_t, negatives int64_t.
inline LiteralNode literal(bool v) { return LiteralNode{v}; }
inline LiteralNode literal(const char* v) {
    return LiteralNode{dstd::String(v)};
}
inline LiteralNode literal(dstd::StringView v) {
    return LiteralNode{dstd::String(v)};
}
inline LiteralNode literal(const dstd::String& v) { return LiteralNode{v}; }

template <typename I,
          std::enable_if_t<std::is_integral_v<std::decay_t<I>> &&
                               !std::is_same_v<std::decay_t<I>, bool>,
                           int> = 0>
LiteralNode literal(I v) {
    std::int64_t s = static_cast<std::int64_t>(v);
    if (s >= 0) return LiteralNode{static_cast<std::uint64_t>(s)};
    return LiteralNode{s};
}

template <typename F,
          std::enable_if_t<std::is_floating_point_v<std::decay_t<F>>, int> = 0>
LiteralNode literal(F v) {
    return LiteralNode{static_cast<double>(v)};
}

}  // namespace detail

/// A query expression under construction. Move-only; owns an AST node.
class Expr {
   public:
    Expr(Expr&&) = default;
    Expr& operator=(Expr&&) = default;
    Expr(const Expr&) = delete;
    Expr& operator=(const Expr&) = delete;

    /// Serialize to the canonical DSL string.
    dstd::String to_string() const { return detail::to_string(*node_); }

    /// Build an executable Query (dstd::Expected<Query, @ref DuqlError>).
    /// Defined in query.h, so include it to call this.
    auto build() const;

   private:
    friend struct detail::QueryAccess;

    explicit Expr(detail::QueryNodePtr node) : node_(std::move(node)) {}

    detail::QueryNodePtr node_;
};

namespace detail {

inline Expr QueryAccess::expr(QueryNodePtr node) {
    return Expr(std::move(node));
}
inline const QueryNode& QueryAccess::node(const Expr& e) { return *e.node_; }
inline QueryNodePtr QueryAccess::release(Expr& e) { return std::move(e.node_); }

}  // namespace detail

/// field op value with an explicit CompareOp and prebuilt literal.
inline Expr field_cmp(dstd::StringView name, CompareOp op, LiteralNode value) {
    return detail::QueryAccess::expr(detail::make_node(
        detail::CompareNode{detail::field_node(name), op, std::move(value)}));
}

#define DFTRACER_UTILS_DUQL_DEFINE_CMP(fn, op_enum)                \
    template <typename T>                                          \
    inline Expr fn(dstd::StringView name, T&& value) {             \
        return field_cmp(name, op_enum,                            \
                         detail::literal(std::forward<T>(value))); \
    }

DFTRACER_UTILS_DUQL_DEFINE_CMP(field_eq, CompareOp::Eq)
DFTRACER_UTILS_DUQL_DEFINE_CMP(field_ne, CompareOp::Ne)
DFTRACER_UTILS_DUQL_DEFINE_CMP(field_gt, CompareOp::Gt)
DFTRACER_UTILS_DUQL_DEFINE_CMP(field_lt, CompareOp::Lt)
DFTRACER_UTILS_DUQL_DEFINE_CMP(field_ge, CompareOp::Ge)
DFTRACER_UTILS_DUQL_DEFINE_CMP(field_le, CompareOp::Le)

#undef DFTRACER_UTILS_DUQL_DEFINE_CMP

/// field in [values].
inline Expr field_in(dstd::StringView name,
                     const dstd::Vector<std::int64_t>& values) {
    detail::ArrayNode arr;
    arr.elements.reserve(values.size());
    for (std::int64_t v : values) arr.elements.push_back(detail::literal(v));
    return detail::QueryAccess::expr(detail::make_node(
        detail::InNode{detail::field_node(name), std::move(arr)}));
}

inline Expr field_in(dstd::StringView name,
                     const dstd::Vector<dstd::String>& values) {
    detail::ArrayNode arr;
    arr.elements.reserve(values.size());
    for (const auto& v : values) arr.elements.push_back(LiteralNode{v});
    return detail::QueryAccess::expr(detail::make_node(
        detail::InNode{detail::field_node(name), std::move(arr)}));
}

/// field not in [values].
inline Expr field_not_in(dstd::StringView name,
                         const dstd::Vector<std::int64_t>& values) {
    detail::ArrayNode arr;
    arr.elements.reserve(values.size());
    for (std::int64_t v : values) arr.elements.push_back(detail::literal(v));
    return detail::QueryAccess::expr(detail::make_node(
        detail::NotInNode{detail::field_node(name), std::move(arr)}));
}

inline Expr field_not_in(dstd::StringView name,
                         const dstd::Vector<dstd::String>& values) {
    detail::ArrayNode arr;
    arr.elements.reserve(values.size());
    for (const auto& v : values) arr.elements.push_back(LiteralNode{v});
    return detail::QueryAccess::expr(detail::make_node(
        detail::NotInNode{detail::field_node(name), std::move(arr)}));
}

/// field like/ilike/~/~*/icontains pattern. The compiled matcher is filled in
/// by Expr::build() (or the C ABI wrap), not here.
inline Expr field_match(dstd::StringView name, MatchOp op,
                        dstd::StringView pattern, bool negated = false) {
    detail::MatchNode node;
    node.field = detail::field_node(name);
    node.op = op;
    node.pattern = dstd::String(pattern);
    node.negated = negated;
    return detail::QueryAccess::expr(detail::make_node(std::move(node)));
}

/// a and b.
inline Expr all_of(Expr a, Expr b) {
    return detail::QueryAccess::expr(detail::make_node(detail::AndNode{
        detail::QueryAccess::release(a), detail::QueryAccess::release(b)}));
}

/// a or b.
inline Expr any_of(Expr a, Expr b) {
    return detail::QueryAccess::expr(detail::make_node(detail::OrNode{
        detail::QueryAccess::release(a), detail::QueryAccess::release(b)}));
}

/// not a.
inline Expr negate(Expr a) {
    return detail::QueryAccess::expr(
        detail::make_node(detail::NotNode{detail::QueryAccess::release(a)}));
}

inline Expr operator&&(Expr a, Expr b) {
    return all_of(std::move(a), std::move(b));
}
inline Expr operator||(Expr a, Expr b) {
    return any_of(std::move(a), std::move(b));
}
inline Expr operator!(Expr a) { return negate(std::move(a)); }

/// A field reference with a fluent, Python-like builder surface, so a query
/// reads as `(Field("cat") == "POSIX") && (Field("dur") > 100)` rather than
/// nested function calls. Each operator/method returns an Expr; combine them
/// with `&&`, `||`, `!`. Mirrors the Python `dftracer.utils.duql.Field` API.
class Field {
   public:
    explicit Field(dstd::StringView name) : name_(name) {}
    /// The same field as `any(<name>)`: a leaf on it holds when any element
    /// of the array holds.
    Field any() const { return Field(dstd::fmt<"any({})">(name_)); }

    template <typename T>
    Expr operator==(T&& v) const {
        return field_eq(name_, std::forward<T>(v));
    }
    template <typename T>
    Expr operator!=(T&& v) const {
        return field_ne(name_, std::forward<T>(v));
    }
    template <typename T>
    Expr operator>(T&& v) const {
        return field_gt(name_, std::forward<T>(v));
    }
    template <typename T>
    Expr operator<(T&& v) const {
        return field_lt(name_, std::forward<T>(v));
    }
    template <typename T>
    Expr operator>=(T&& v) const {
        return field_ge(name_, std::forward<T>(v));
    }
    template <typename T>
    Expr operator<=(T&& v) const {
        return field_le(name_, std::forward<T>(v));
    }

    Expr in(const dstd::Vector<std::int64_t>& values) const {
        return field_in(name_, values);
    }
    Expr in(const dstd::Vector<dstd::String>& values) const {
        return field_in(name_, values);
    }
    Expr not_in(const dstd::Vector<std::int64_t>& values) const {
        return field_not_in(name_, values);
    }
    Expr not_in(const dstd::Vector<dstd::String>& values) const {
        return field_not_in(name_, values);
    }

    Expr like(dstd::StringView pattern) const {
        return field_match(name_, MatchOp::Like, pattern);
    }
    Expr ilike(dstd::StringView pattern) const {
        return field_match(name_, MatchOp::Ilike, pattern);
    }
    Expr regex(dstd::StringView pattern) const {
        return field_match(name_, MatchOp::Regex, pattern);
    }
    Expr iregex(dstd::StringView pattern) const {
        return field_match(name_, MatchOp::Iregex, pattern);
    }
    Expr contains(dstd::StringView substring) const {
        return field_match(name_, MatchOp::Icontains, substring);
    }

   private:
    dstd::String name_;
};

/// Call-form field shorthand matching Python's `F("args.level")`:
/// `F("dur") < 25` is exactly `Field("dur") < 25`.
struct FieldFactory {
    Field operator()(dstd::StringView name) const { return Field(name); }
};
inline constexpr FieldFactory F{};

namespace detail {

inline dstd::String string_literal(dstd::StringView v) {
    return quote_string(v);
}

/// The text of `v`. A number that is not finite sets `error`, when it holds
/// none yet, and gives `null`.
inline dstd::String number_text(double v, dstd::String& error) {
    if (!std::isfinite(v)) {
        if (error.empty()) error = "duql: a number must be finite";
        return "null";
    }
    dstd::String s = dstd::double_text(v);
    if (s.find_first_of(".eE") == dstd::String::npos) s += ".0";
    return s;
}

/// The input error of a builder value, or of every value in a list; empty
/// when there is none.
template <typename T>
dstd::String error_of(const T& value) {
    return value.error();
}
template <typename T>
dstd::String error_of(const dstd::Vector<T>& values) {
    for (const auto& v : values) {
        dstd::String e = error_of(v);
        if (!e.empty()) return e;
    }
    return {};
}
template <typename T>
dstd::String error_of(const dstd::Optional<T>& value) {
    return value ? error_of(*value) : dstd::String();
}

inline dstd::String error_of(const dstd::String& error) { return error; }

/// The first non-empty input error among `values`, in argument order.
inline dstd::String first_error() { return {}; }
template <typename T, typename... Rest>
dstd::String first_error(const T& value, const Rest&... rest) {
    dstd::String e = error_of(value);
    return e.empty() ? first_error(rest...) : e;
}

}  // namespace detail

class Pipe;

/// A duql expression under construction, held as duql text. Every builder
/// call returns a new value; nothing runs until a Pipe is executed.
/// Converts implicitly from a literal: `c("dur") > 5`, `c("name") == "read"`.
/// A value keeps the first input error of the calls that built it, such as a
/// number that is not finite or an index on a value that is not a field path.
/// The error passes to every value built from it and comes out of
/// Pipe::text() or Source::text().
class Col {
   public:
    Col(std::nullptr_t) : text_("null") {}
    Col(bool v) : text_(v ? "true" : "false") {}
    template <typename I,
              std::enable_if_t<
                  std::is_integral_v<I> && !std::is_same_v<I, bool>, int> = 0>
    Col(I v) : text_(dstd::int_text(v)) {}
    template <typename D,
              std::enable_if_t<std::is_floating_point_v<D>, int> = 0>
    Col(D v) : text_(detail::number_text(static_cast<double>(v), error_)) {}
    Col(const char* v) : text_(detail::string_literal(v)) {}
    Col(dstd::StringView v) : text_(detail::string_literal(v)) {}
    Col(const dstd::String& v) : text_(detail::string_literal(v)) {}

    /// A value from its duql text, unchecked; the parser checks it. `error`
    /// is the input error the value starts with, empty for none.
    static Col from_text(dstd::String text, dstd::String error = {}) {
        Col c(nullptr);
        c.text_ = std::move(text);
        c.error_ = std::move(error);
        return c;
    }

    const dstd::String& raw() const { return text_; }
    /// The first input error, empty when there is none.
    const dstd::String& error() const { return error_; }

    /// `path[index]`: `index` is an int, `param("n")` or any expression. An
    /// index follows a field path, so any other value gives an error value.
    Col operator[](const Col& index) const {
        dstd::String e = detail::first_error(*this, index);
        if (e.empty() && !is_path()) e = "an index follows a field path";
        return from_text(dstd::fmt<"{}[{}]">(text_, index.text_), std::move(e));
    }

    Col is_in(const dstd::Vector<Col>& values) const;
    Col not_in(const dstd::Vector<Col>& values) const;
    /// `x in $p`: `list` is `param("p")`, bound to a list.
    Col is_in(const Col& list) const {
        return wrap(dstd::fmt<"{} in {}">(text_, list.text_),
                    detail::first_error(*this, list));
    }
    Col not_in(const Col& list) const {
        return wrap(dstd::fmt<"{} not in {}">(text_, list.text_),
                    detail::first_error(*this, list));
    }
    Col is_in(const Pipe& subquery) const;
    Col not_in(const Pipe& subquery) const;
    Col between(const Col& low, const Col& high) const {
        return wrap(
            dstd::fmt<"{} between {} and {}">(text_, low.text_, high.text_),
            detail::first_error(*this, low, high));
    }
    Col not_between(const Col& low, const Col& high) const {
        return wrap(
            dstd::fmt<"{} not between {} and {}">(text_, low.text_, high.text_),
            detail::first_error(*this, low, high));
    }
    /// A pattern is a string or `param("p")` bound to a string.
    Col like(const Col& pattern,
             dstd::Optional<dstd::StringView> escape = dstd::nullopt) const {
        return match("like", pattern, escape);
    }
    Col ilike(const Col& pattern,
              dstd::Optional<dstd::StringView> escape = dstd::nullopt) const {
        return match("ilike", pattern, escape);
    }
    Col not_like(const Col& pattern, dstd::Optional<dstd::StringView> escape =
                                         dstd::nullopt) const {
        return match("not like", pattern, escape);
    }
    Col not_ilike(const Col& pattern, dstd::Optional<dstd::StringView> escape =
                                          dstd::nullopt) const {
        return match("not ilike", pattern, escape);
    }
    Col regex(const Col& pattern) const { return binary("~", pattern); }
    Col iregex(const Col& pattern) const { return binary("~*", pattern); }
    Col not_regex(const Col& pattern) const { return binary("!~", pattern); }
    Col not_iregex(const Col& pattern) const { return binary("!~*", pattern); }
    /// `<this> over n rows`, a rows frame on a window call.
    Col over_rows(std::int64_t n) const {
        return from_text(dstd::fmt<"{} over {} rows">(text_, n), error_);
    }
    /// `<this> over <width>`, a range frame on a window call; `width` is a
    /// number, `duration(...)` or `param(...)`.
    Col over(const Col& width) const {
        return from_text(dstd::fmt<"{} over {}">(text_, width.text_),
                         detail::first_error(*this, width));
    }
    Col is_null() const { return wrap(dstd::fmt<"{} is null">(text_), error_); }
    Col is_not_null() const {
        return wrap(dstd::fmt<"{} is not null">(text_), error_);
    }
    Col is_missing() const {
        return wrap(dstd::fmt<"{} is missing">(text_), error_);
    }
    Col is_not_missing() const {
        return wrap(dstd::fmt<"{} is not missing">(text_), error_);
    }
    Col coalesce(const Col& other) const { return binary("??", other); }
    /// The legacy case-insensitive substring test `"text" in path`, or
    /// `"text" in any(path)`; this value must be a path.
    Col icontains(dstd::StringView text, bool any = false) const {
        return contains(text, any, false);
    }
    Col not_icontains(dstd::StringView text, bool any = false) const {
        return contains(text, any, true);
    }
    /// `this -> rowset.path`, or `this -> rowset(key).path`.
    Col ref(dstd::StringView rowset, dstd::StringView path,
            dstd::StringView key = {}) const {
        dstd::String t = dstd::fmt<"({} -> {}">(text_, rowset);
        if (!key.empty()) t += dstd::fmt<"({})">(key);
        return from_text(dstd::fmt<"{}.{})">(t, path), error_);
    }

    Col binary(dstd::StringView op, const Col& right) const {
        return wrap(dstd::fmt<"{} {} {}">(text_, op, right.text_),
                    detail::first_error(*this, right));
    }

   private:
    static Col wrap(const dstd::String& t, dstd::String error) {
        return from_text(dstd::fmt<"({})">(t), std::move(error));
    }
    Col match(dstd::StringView op, const Col& pattern,
              dstd::Optional<dstd::StringView> escape) const {
        dstd::String t = dstd::fmt<"{} {} {}">(text_, op, pattern.text_);
        if (escape)
            t += dstd::fmt<" escape {}">(detail::string_literal(*escape));
        return wrap(t, detail::first_error(*this, pattern));
    }
    Col contains(dstd::StringView text, bool any, bool negated) const {
        dstd::String t = detail::string_literal(text);
        t += negated ? " not in " : " in ";
        t += any ? dstd::fmt<"any({})">(text_) : text_;
        return wrap(t, error_);
    }

    bool is_path() const {
        if (text_.empty() || (text_[0] >= '0' && text_[0] <= '9')) return false;
        int depth = 0;
        bool tick = false;
        for (char ch : text_) {
            if (ch == '`') {
                tick = !tick;
            } else if (tick) {
                continue;
            } else if (ch == '[') {
                ++depth;
            } else if (ch == ']') {
                --depth;
            } else if (depth == 0 &&
                       !(ch == '_' || ch == '.' || ch == '^' ||
                         (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                         (ch >= 'A' && ch <= 'Z'))) {
                return false;
            }
        }
        return true;
    }

    // Before text_: the float constructor fills it while text_ is built.
    dstd::String error_;
    dstd::String text_;
};

/// A record path as written in duql: `a.b[0]`, `.x`, `^.x`, backtick keys.
inline Col c(dstd::StringView path) {
    return Col::from_text(dstd::String(path));
}
inline Col lit(const Col& v) { return v; }
inline Col param(dstd::StringView name) {
    return Col::from_text(dstd::fmt<"${}">(name));
}
/// `amount` followed by a unit: ns, us, ms, s, m or h.
template <typename N, std::enable_if_t<std::is_arithmetic_v<N>, int> = 0>
Col duration(N amount, dstd::StringView unit) {
    dstd::String t;
    dstd::String error;
    if constexpr (std::is_integral_v<N>)
        t = dstd::int_text(amount);
    else
        t = detail::number_text(static_cast<double>(amount), error);
    return Col::from_text(dstd::fmt<"{}{}">(t, unit), std::move(error));
}

/// A named value: a `derive`, `agg` or `group` output, or a named argument.
struct Named {
    dstd::String name;
    Col value;
    const dstd::String& error() const { return value.error(); }
};

inline dstd::String join(const dstd::Vector<Col>& items) {
    dstd::String out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out += ", ";
        out += items[i].raw();
    }
    return out;
}

/// `name(args, named = value, ...)`; `name` may be namespaced (`ns.f`).
inline Col fn(dstd::StringView name, const dstd::Vector<Col>& args = {},
              const dstd::Vector<Named>& named = {}) {
    dstd::String t = dstd::fmt<"{}({}">(name, join(args));
    for (std::size_t i = 0; i < named.size(); ++i) {
        if (i || !args.empty()) t += ", ";
        t += dstd::fmt<"{} = {}">(named[i].name, named[i].value.raw());
    }
    return Col::from_text(dstd::fmt<"{})">(t),
                          detail::first_error(args, named));
}
inline Col tup(const dstd::Vector<Col>& items) {
    return Col::from_text(dstd::fmt<"({})">(join(items)),
                          detail::error_of(items));
}
inline Col list(const dstd::Vector<Col>& items) {
    return Col::from_text(dstd::fmt<"[{}]">(join(items)),
                          detail::error_of(items));
}
/// `case { cond => value, ..., else => otherwise }`; a missing `otherwise`
/// is null.
inline Col case_(const dstd::Vector<dstd::Pair<Col, Col>>& pairs,
                 const dstd::Optional<Col>& otherwise = dstd::nullopt) {
    dstd::Vector<Col> args;
    args.reserve(pairs.size() * 2 + 1);
    for (const auto& [cond, value] : pairs) {
        args.push_back(cond);
        args.push_back(value);
    }
    args.push_back(otherwise ? *otherwise : Col(nullptr));
    return fn("case", args);
}

inline Col operator==(const Col& a, const Col& b) { return a.binary("==", b); }
inline Col operator!=(const Col& a, const Col& b) { return a.binary("!=", b); }
inline Col operator<(const Col& a, const Col& b) { return a.binary("<", b); }
inline Col operator<=(const Col& a, const Col& b) { return a.binary("<=", b); }
inline Col operator>(const Col& a, const Col& b) { return a.binary(">", b); }
inline Col operator>=(const Col& a, const Col& b) { return a.binary(">=", b); }
inline Col operator+(const Col& a, const Col& b) { return a.binary("+", b); }
inline Col operator-(const Col& a, const Col& b) { return a.binary("-", b); }
inline Col operator*(const Col& a, const Col& b) { return a.binary("*", b); }
inline Col operator/(const Col& a, const Col& b) { return a.binary("/", b); }
inline Col operator%(const Col& a, const Col& b) { return a.binary("%", b); }
/// Integer division, `//`.
inline Col idiv(const Col& a, const Col& b) { return a.binary("//", b); }
inline Col operator&&(const Col& a, const Col& b) { return a.binary("and", b); }
inline Col operator||(const Col& a, const Col& b) { return a.binary("or", b); }
inline Col operator!(const Col& a) {
    return Col::from_text(dstd::fmt<"(not {})">(a.raw()), a.error());
}
/// Negation; in a sort key it sorts descending, as `-x` does in the text.
inline Col operator-(const Col& a) {
    return Col::from_text(dstd::fmt<"-({})">(a.raw()), a.error());
}

/// A `select`, `group`, `window` or `session` item: a path (from a string),
/// an expression, or `name = expression`.
struct Item {
    Item(const char* path) : value(c(path)) {}
    Item(dstd::StringView path) : value(c(path)) {}
    Item(const dstd::String& path) : value(c(path)) {}
    Item(Col v) : value(std::move(v)) {}
    Item(dstd::String n, Col v) : value(std::move(v)), name(std::move(n)) {}
    Col value;
    dstd::String name;
    const dstd::String& error() const { return value.error(); }
};

enum class Nulls : std::uint8_t { Default, First, Last };

/// A sort key: a path (from a string) or an expression, ascending; `-x`
/// sorts descending.
struct SortKey {
    SortKey(const char* path) : value(c(path)) {}
    SortKey(const dstd::String& path) : value(c(path)) {}
    SortKey(Col v, Nulls n = Nulls::Default) : value(std::move(v)), nulls(n) {}
    Col value;
    Nulls nulls = Nulls::Default;
    const dstd::String& error() const { return value.error(); }
};

/// A lookup key: `left`, or `left == right` (`right` the row set's side).
struct JoinKey {
    JoinKey(const char* path) : left(c(path)) {}
    JoinKey(const dstd::String& path) : left(c(path)) {}
    JoinKey(Col l) : left(std::move(l)) {}
    JoinKey(Col l, Col r) : left(std::move(l)), right(std::move(r)) {}
    Col left;
    dstd::Optional<Col> right;
    dstd::String error() const { return detail::first_error(left, right); }
};

enum class Asof : std::uint8_t { Backward, Forward, Nearest };

/// How `lookup` treats rows: LEFT keeps every row, INNER drops rows with no
/// match, ANTI keeps only rows with no match (and takes no `into`).
enum class LookupHow : std::uint8_t { Left, Inner, Anti };

/// A duql query under construction, held as duql text with its bound
/// parameters. Stage methods return a new Pipe. A Pipe with no source reads
/// the data of the View that runs it. A Pipe keeps the first input error of
/// the calls that built it, such as a @ref time_range with no bound, or the
/// error of a @ref Col it took; text() returns it.
class Pipe {
   public:
    Pipe() = default;

    /// The builder's duql text, not canonical.
    dstd::String raw() const;
    /// The canonical duql text, the engine's `duql MAJOR.MINOR` first. Fails
    /// with the first input error of the builder calls, or with the parser's
    /// error when the text does not parse (a malformed path or call). Defined
    /// in the duql library, not in this header.
    dstd::Expected<dstd::String, DuqlError> text() const;
    /// The first input error of the builder calls, empty when there is none.
    const dstd::String& error() const { return error_; }
    /// The bound parameters, by name without the `$`.
    const dstd::Vector<dstd::Pair<dstd::String, ParamValue>>& bound() const {
        return params_;
    }
    /// Binds `$name` to a scalar, or to a list for `x in $name`.
    Pipe bind(dstd::String name, ParamValue value) const {
        Pipe p = *this;
        for (auto& kv : p.params_)
            if (kv.first == name) {
                kv.second = std::move(value);
                return p;
            }
        p.params_.emplace_back(std::move(name), std::move(value));
        return p;
    }

    Pipe let(dstd::StringView name, const Pipe& pipeline) const {
        return decl(dstd::fmt<"let {} = {}">(name, pipeline.inline_text()),
                    pipeline.error_);
    }
    Pipe define(dstd::StringView name, const dstd::Vector<dstd::String>& params,
                const Col& body) const {
        return define_text(name, params, body.raw(), body.error());
    }
    /// `def name(params) = stages;`: a pipeline macro. `body` must not start
    /// with a source.
    Pipe define(dstd::StringView name, const dstd::Vector<dstd::String>& params,
                const Pipe& body) const {
        return define_text(name, params, body.inline_text(), body.error_);
    }
    /// A stage that calls the pipeline macro `name` with `args`.
    Pipe use(dstd::StringView name, const dstd::Vector<Col>& args = {}) const {
        dstd::String t = dstd::fmt<"{}(">(name);
        for (std::size_t i = 0; i < args.size(); ++i)
            t += (i ? ", " : "") + args[i].raw();
        return stage(dstd::fmt<"{})">(t), detail::error_of(args));
    }
    Pipe define_text(dstd::StringView name,
                     const dstd::Vector<dstd::String>& params,
                     const dstd::String& body,
                     const dstd::String& error = {}) const {
        dstd::String t = dstd::fmt<"def {}">(name);
        if (!params.empty()) {
            t += "(";
            for (std::size_t i = 0; i < params.size(); ++i)
                t += (i ? ", " : "") + params[i];
            t += ")";
        }
        return decl(dstd::fmt<"{} = {}">(t, body), error);
    }

    Pipe where(const Col& condition) const {
        return stage(dstd::fmt<"where {}">(condition.raw()), condition.error());
    }
    Pipe where(const Expr& condition) const {
        return stage(dstd::fmt<"where ({})">(condition.to_string()));
    }
    Pipe derive(const dstd::Vector<Named>& fields) const {
        return stage(dstd::fmt<"derive {}">(assigns(fields)),
                     detail::error_of(fields));
    }
    Pipe select(const dstd::Vector<Item>& items) const {
        return stage(dstd::fmt<"select {}">(items_text(items)),
                     detail::error_of(items));
    }
    Pipe drop(const dstd::Vector<dstd::String>& paths) const {
        return stage(dstd::fmt<"drop {}">(names(paths)));
    }
    /// `rename new = old, ...`.
    Pipe rename(const dstd::Vector<dstd::Pair<dstd::String, dstd::String>>&
                    pairs) const {
        dstd::String t = "rename ";
        for (std::size_t i = 0; i < pairs.size(); ++i)
            t += dstd::fmt<"{}{} = {}">((i ? ", " : ""), pairs[i].first,
                                        pairs[i].second);
        return stage(t);
    }
    Pipe distinct(const dstd::Vector<Item>& keys = {}) const {
        return stage(keys.empty() ? "distinct"
                                  : dstd::fmt<"distinct {}">(items_text(keys)),
                     detail::error_of(keys));
    }
    Pipe group(const dstd::Vector<Item>& keys,
               const dstd::Vector<Named>& aggregates) const {
        dstd::String t = "group ";
        if (!keys.empty()) t += dstd::fmt<"{} ">(items_text(keys));
        return stage(t + block(aggregates),
                     detail::first_error(keys, aggregates));
    }
    Pipe agg(const dstd::Vector<Named>& aggregates) const {
        return stage(dstd::fmt<"agg {}">(block(aggregates)),
                     detail::error_of(aggregates));
    }
    Pipe window(const dstd::Vector<Item>& partition,
                const dstd::Vector<SortKey>& order,
                const dstd::Vector<Named>& fields) const {
        dstd::String t = "window ";
        if (!partition.empty()) t += dstd::fmt<"{} ">(items_text(partition));
        if (!order.empty()) t += dstd::fmt<"sort {} ">(sort_keys(order));
        return stage(t + block(fields),
                     detail::first_error(partition, order, fields));
    }
    /// `labels` pairs with `values`; a non-empty label names that column.
    Pipe pivot(const Col& key, const dstd::Vector<Col>& values,
               const dstd::Vector<Named>& aggregates,
               const dstd::Vector<dstd::String>& labels = {}) const {
        dstd::String t = dstd::fmt<"pivot {}">(key.raw());
        if (!values.empty()) {
            t += " in [";
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (i) t += ", ";
                t += values[i].raw();
                if (i < labels.size() && !labels[i].empty())
                    t += dstd::fmt<" as {}">(labels[i]);
            }
            t += "]";
        }
        return stage(dstd::fmt<"{} {}">(t, block(aggregates)),
                     detail::first_error(key, values, aggregates));
    }
    Pipe unpivot(const dstd::Vector<dstd::String>& paths,
                 dstd::StringView key_name, dstd::StringView value_name) const {
        return stage(dstd::fmt<"unpivot {} as {}, {}">(names(paths), key_name,
                                                       value_name));
    }
    Pipe sort(const dstd::Vector<SortKey>& keys) const {
        return stage(dstd::fmt<"sort {}">(sort_keys(keys)),
                     detail::error_of(keys));
    }
    Pipe take(const Col& count, const dstd::Vector<Col>& by = {},
              const dstd::Vector<SortKey>& order = {}) const {
        dstd::String t = dstd::fmt<"take {}">(count.raw());
        if (!by.empty()) t += dstd::fmt<" by {}">(join(by));
        if (!order.empty()) t += dstd::fmt<" sort {}">(sort_keys(order));
        return stage(t, detail::first_error(count, by, order));
    }
    /// `take first..last`: rows `first` to `last`, 1-based and inclusive.
    Pipe take_range(const Col& first, const Col& last) const {
        return stage(dstd::fmt<"take {}..{}">(first.raw(), last.raw()),
                     detail::first_error(first, last));
    }
    Pipe skip(const Col& count) const {
        return stage(dstd::fmt<"skip {}">(count.raw()), count.error());
    }
    Pipe sample(const Col& amount, bool percent = false,
                dstd::Optional<Col> seed = dstd::nullopt) const {
        dstd::String t =
            dstd::fmt<"sample {}{}">(amount.raw(), (percent ? "%" : ""));
        if (seed) t += dstd::fmt<" seed {}">(seed->raw());
        return stage(t, detail::first_error(amount, seed));
    }
    Pipe expand(dstd::StringView path, dstd::StringView as = {},
                dstd::StringView with_index = {},
                bool keep_empty = false) const {
        dstd::String t = dstd::fmt<"expand {}">(path);
        if (!as.empty()) t += dstd::fmt<" as {}">(as);
        if (!with_index.empty()) t += dstd::fmt<" with_index {}">(with_index);
        if (keep_empty) t += " keep_empty";
        return stage(t);
    }
    /// `parse col ~ pattern`; `pattern` is a string or `param("p")` bound to a
    /// string, and its named groups become columns.
    Pipe parse(const Col& col, const Col& pattern) const {
        return stage(dstd::fmt<"parse {} ~ {}">(col.raw(), pattern.raw()),
                     detail::first_error(col, pattern));
    }
    Pipe lookup(dstd::StringView rowset, const dstd::Vector<JoinKey>& on,
                dstd::StringView into = {},
                LookupHow how = LookupHow::Left) const {
        return lookup_text(dstd::String(rowset), {}, on, into, how);
    }
    /// `side` needs a source; it prints as `(from ...)`.
    Pipe lookup(const Pipe& side, const dstd::Vector<JoinKey>& on,
                dstd::StringView into = {},
                LookupHow how = LookupHow::Left) const {
        return lookup_text(dstd::fmt<"({})">(side.inline_text()), side.error_,
                           on, into, how);
    }
    Pipe lookup_asof(dstd::StringView rowset, const dstd::Vector<JoinKey>& on,
                     const JoinKey& time, Asof direction = Asof::Backward,
                     dstd::Optional<Col> within = dstd::nullopt) const {
        return asof_text(dstd::String(rowset), {}, on, time, direction, within);
    }
    Pipe lookup_asof(const Pipe& side, const dstd::Vector<JoinKey>& on,
                     const JoinKey& time, Asof direction = Asof::Backward,
                     dstd::Optional<Col> within = dstd::nullopt) const {
        return asof_text(dstd::fmt<"({})">(side.inline_text()), side.error_, on,
                         time, direction, within);
    }
    Pipe lookup_overlap(dstd::StringView rowset,
                        const dstd::Vector<JoinKey>& on,
                        dstd::StringView into = {}) const {
        return overlap_text(dstd::String(rowset), {}, on, into);
    }
    Pipe lookup_overlap(const Pipe& side, const dstd::Vector<JoinKey>& on,
                        dstd::StringView into = {}) const {
        return overlap_text(dstd::fmt<"({})">(side.inline_text()), side.error_,
                            on, into);
    }
    /// `union (other)`; `other` needs a source.
    Pipe union_with(const Pipe& other) const {
        return stage(dstd::fmt<"union ({})">(other.inline_text()),
                     other.error_);
    }
    /// `union name`: the rows of the row set or `let` `name`.
    Pipe union_with(dstd::StringView name) const {
        return stage(dstd::fmt<"union {}">(name));
    }
    Pipe call(const Col& call) const {
        return stage(dstd::fmt<"call {}">(call.raw()), call.error());
    }
    /// `time_range low .. high`; an absent bound leaves that side open.
    Pipe time_range(dstd::Optional<Col> low, dstd::Optional<Col> high,
                    bool overlap = false) const {
        if (!low && !high)
            return failed("duql: time_range needs a low or a high bound");
        dstd::String t = "time_range ";
        if (low) t += dstd::fmt<"{} ">(low->raw());
        t += "..";
        if (high) t += dstd::fmt<" {}">(high->raw());
        return stage(t + (overlap ? " overlap" : ""),
                     detail::first_error(low, high));
    }
    Pipe call_tree() const { return stage("call_tree"); }
    /// `bucket width [every e] [at t] [fill [mode] [from low to high]] [as
    /// name]`; `mode` is "zero", "forward" or "linear", and a mode or range
    /// needs `fill`.
    Pipe bucket(const Col& width, bool fill = false, dstd::StringView as = {},
                dstd::StringView mode = "zero",
                dstd::Optional<Col> low = dstd::nullopt,
                dstd::Optional<Col> high = dstd::nullopt,
                dstd::Optional<Col> every = dstd::nullopt,
                dstd::Optional<Col> at = dstd::nullopt) const {
        dstd::String error = detail::first_error(width, low, high, every, at);
        if (error.empty()) {
            if (mode != "zero" && mode != "forward" && mode != "linear")
                error = "duql: bucket mode is zero, forward or linear";
            else if (!fill && (mode != "zero" || low || high))
                error = "duql: a bucket mode or range needs fill";
            else if (low.has_value() != high.has_value())
                error = "duql: a bucket range needs both low and high";
        }
        if (!error.empty()) return failed(std::move(error));
        dstd::String t = dstd::fmt<"bucket {}">(width.raw());
        if (every) t += dstd::fmt<" every {}">(every->raw());
        if (at) t += dstd::fmt<" at {}">(at->raw());
        if (fill) t += " fill";
        if (mode != "zero") t += dstd::fmt<" {}">(mode);
        if (low) t += dstd::fmt<" from {} to {}">(low->raw(), high->raw());
        if (!as.empty()) t += dstd::fmt<" as {}">(as);
        return stage(t);
    }
    Pipe session(const dstd::Vector<Item>& keys, const Col& gap,
                 dstd::Optional<Col> max = dstd::nullopt,
                 dstd::StringView as = {}) const {
        dstd::String t = "session ";
        if (!keys.empty()) t += dstd::fmt<"{} ">(items_text(keys));
        t += dstd::fmt<"gap {}">(gap.raw());
        if (max) t += dstd::fmt<" max {}">(max->raw());
        if (!as.empty()) t += dstd::fmt<" as {}">(as);
        return stage(t, detail::first_error(keys, gap, max));
    }

    /// The pipeline on one line, stages joined by ` | `, without declarations.
    dstd::String inline_text() const {
        dstd::String out = from_;
        for (const auto& s : stages_) {
            if (!out.empty()) out += " | ";
            out += s;
        }
        return out;
    }

   private:
    friend Pipe source(dstd::StringView file);
    friend Pipe sources(const dstd::Vector<dstd::String>& files);
    friend Pipe rowset(dstd::StringView name);
    friend Pipe source_param(dstd::StringView name);

    // A call with an input error keeps the first one; the pipe still grows.
    Pipe stage(dstd::String text, const dstd::String& error = {}) const {
        Pipe p = *this;
        p.stages_.push_back(std::move(text));
        p.note(error);
        return p;
    }
    Pipe decl(dstd::String text, const dstd::String& error = {}) const {
        Pipe p = *this;
        p.decls_.push_back(std::move(text));
        p.note(error);
        return p;
    }
    Pipe failed(dstd::String error) const {
        Pipe p = *this;
        p.note(error);
        return p;
    }
    void note(const dstd::String& error) {
        if (error_.empty()) error_ = error;
    }
    static dstd::String names(const dstd::Vector<dstd::String>& list) {
        dstd::String out;
        for (std::size_t i = 0; i < list.size(); ++i)
            out += (i ? ", " : "") + list[i];
        return out;
    }
    static dstd::String assigns(const dstd::Vector<Named>& fields) {
        dstd::String out;
        for (std::size_t i = 0; i < fields.size(); ++i)
            out += dstd::fmt<"{}{} = {}">((i ? ", " : ""), fields[i].name,
                                          fields[i].value.raw());
        return out;
    }
    static dstd::String block(const dstd::Vector<Named>& fields) {
        return fields.empty() ? "{ }" : dstd::fmt<"{ {} }">(assigns(fields));
    }
    static dstd::String items_text(const dstd::Vector<Item>& items) {
        dstd::String out;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i) out += ", ";
            if (!items[i].name.empty())
                out += dstd::fmt<"{} = ">(items[i].name);
            out += items[i].value.raw();
        }
        return out;
    }
    static dstd::String sort_keys(const dstd::Vector<SortKey>& keys) {
        dstd::String out;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (i) out += ", ";
            out += keys[i].value.raw();
            if (keys[i].nulls == Nulls::First) out += " nulls first";
            if (keys[i].nulls == Nulls::Last) out += " nulls last";
        }
        return out;
    }
    static dstd::String key_text(const JoinKey& k) {
        return k.right ? dstd::fmt<"{} == {}">(k.left.raw(), k.right->raw())
                       : k.left.raw();
    }
    Pipe lookup_text(const dstd::String& side, const dstd::String& side_error,
                     const dstd::Vector<JoinKey>& on, dstd::StringView into,
                     LookupHow how) const {
        dstd::String error = detail::first_error(side_error, on);
        if (error.empty() && how == LookupHow::Anti && !into.empty())
            error = "duql: an anti lookup cannot take into";
        if (!error.empty()) return failed(std::move(error));
        dstd::String t = join_keys(side, on);
        if (how == LookupHow::Inner) t += " inner";
        if (how == LookupHow::Anti) t += " anti";
        if (!into.empty()) t += dstd::fmt<" into {}">(into);
        return stage(t);
    }
    Pipe asof_text(const dstd::String& side, const dstd::String& side_error,
                   const dstd::Vector<JoinKey>& on, const JoinKey& time,
                   Asof direction, const dstd::Optional<Col>& within) const {
        dstd::String t =
            dstd::fmt<"{} asof {}">(join_keys(side, on), key_text(time));
        if (direction == Asof::Forward) t += " forward";
        if (direction == Asof::Nearest) t += " nearest";
        if (within) t += dstd::fmt<" within {}">(within->raw());
        return stage(t, detail::first_error(side_error, on, time, within));
    }
    Pipe overlap_text(const dstd::String& side, const dstd::String& side_error,
                      const dstd::Vector<JoinKey>& on,
                      dstd::StringView into) const {
        dstd::String t = dstd::fmt<"{} overlap">(join_keys(side, on));
        if (!into.empty()) t += dstd::fmt<" into {}">(into);
        return stage(t, detail::first_error(side_error, on));
    }
    static dstd::String join_keys(const dstd::String& rowset,
                                  const dstd::Vector<JoinKey>& on) {
        dstd::String t = dstd::fmt<"lookup {} on ">(rowset);
        for (std::size_t i = 0; i < on.size(); ++i)
            t += (i ? ", " : "") + key_text(on[i]);
        return t;
    }

    dstd::String error_;
    dstd::String from_;
    dstd::Vector<dstd::String> decls_;
    dstd::Vector<dstd::String> stages_;
    dstd::Vector<dstd::Pair<dstd::String, ParamValue>> params_;
};

/// A pipeline over the trace file `file`: `from "file"`.
inline Pipe source(dstd::StringView file) {
    Pipe p;
    p.from_ = dstd::fmt<"from {}">(detail::string_literal(file));
    return p;
}
/// A pipeline over several trace files.
inline Pipe sources(const dstd::Vector<dstd::String>& files) {
    Pipe p;
    p.from_ = "from ";
    for (std::size_t i = 0; i < files.size(); ++i)
        p.from_ += (i ? ", " : "") + detail::string_literal(files[i]);
    return p;
}
/// A pipeline over the row set or `let` `name` (`all`, `data`, ...).
inline Pipe rowset(dstd::StringView name) {
    Pipe p;
    p.from_ = dstd::fmt<"from {}">(name);
    return p;
}
/// A pipeline over the file bound to parameter `name`: `from $name`.
inline Pipe source_param(dstd::StringView name) {
    Pipe p;
    p.from_ = dstd::fmt<"from ${}">(name);
    return p;
}

/// A scalar sub-query: the only cell of `pipeline`, which needs a source.
inline Col sub(const Pipe& pipeline) {
    return Col::from_text(dstd::fmt<"({})">(pipeline.inline_text()),
                          pipeline.error());
}

inline Col Col::is_in(const dstd::Vector<Col>& values) const {
    return wrap(dstd::fmt<"{} in [{}]">(text_, join(values)),
                detail::first_error(*this, values));
}
inline Col Col::not_in(const dstd::Vector<Col>& values) const {
    return wrap(dstd::fmt<"{} not in [{}]">(text_, join(values)),
                detail::first_error(*this, values));
}
inline Col Col::is_in(const Pipe& subquery) const {
    return wrap(dstd::fmt<"{} in ({})">(text_, subquery.inline_text()),
                detail::first_error(*this, subquery));
}
inline Col Col::not_in(const Pipe& subquery) const {
    return wrap(dstd::fmt<"{} not in ({})">(text_, subquery.inline_text()),
                detail::first_error(*this, subquery));
}

inline dstd::String Pipe::raw() const {
    dstd::String out;
    for (const auto& d : decls_) out += dstd::fmt<"{};\n">(d);
    return out + inline_text();
}

/// The member text of a duql source, in the order added. Each method returns
/// a new Source.
/// A Source keeps the first input error of the calls that built it, such as a
/// row set that starts with a source, or the error of a @ref Pipe or
/// @ref Col it took; text() returns it.
class Source {
   public:
    /// `name = pipeline`. A `pipeline` that starts with a source (`from ...`)
    /// gives an error: a source member reads the data, not a file.
    Source rowset(dstd::StringView name, const Pipe& pipeline) const {
        const dstd::String body = pipeline.inline_text();
        if (pipeline.error().empty() && body.rfind("from ", 0) == 0)
            return failed("a source row set cannot start with from");
        return add(dstd::fmt<"{} = {}">(name, body), pipeline.error());
    }
    /// `def name(params) = body`: a column macro.
    Source define(dstd::StringView name,
                  const dstd::Vector<dstd::String>& params,
                  const Col& body) const {
        return add(Pipe().define(name, params, body));
    }
    /// `def name(params) = stages`: a pipeline macro; `body` must not start
    /// with a source.
    Source define(dstd::StringView name,
                  const dstd::Vector<dstd::String>& params,
                  const Pipe& body) const {
        return add(Pipe().define(name, params, body));
    }
    /// `def name = true` or `def name = false`.
    Source flag(dstd::StringView name, bool value) const {
        return add(
            dstd::fmt<"def {}{}">(name, (value ? " = true" : " = false")));
    }
    /// The members as canonical text joined by `;\n`, the form merge_source
    /// stores. Fails with the first input error of the builder calls, or with
    /// the parser's error when a member does not parse. Defined in the duql
    /// library.
    dstd::Expected<dstd::String, DuqlError> text() const;
    /// The first input error of the builder calls, empty when there is none.
    const dstd::String& error() const { return error_; }

   private:
    Source add(const Pipe& decl) const {
        dstd::String t = decl.raw();
        t.resize(t.size() - 2);
        return add(std::move(t), decl.error());
    }
    Source add(dstd::String member, const dstd::String& error = {}) const {
        Source s = *this;
        s.members_.push_back(std::move(member));
        s.note(error);
        return s;
    }
    Source failed(dstd::String error) const {
        Source s = *this;
        s.note(error);
        return s;
    }
    void note(const dstd::String& error) {
        if (error_.empty()) error_ = error;
    }
    dstd::String error_;
    dstd::Vector<dstd::String> members_;
};

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_BUILDER_H
