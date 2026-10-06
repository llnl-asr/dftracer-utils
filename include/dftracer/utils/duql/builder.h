#ifndef DFTRACER_UTILS_DUQL_BUILDER_H
#define DFTRACER_UTILS_DUQL_BUILDER_H

#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/duql/ast.h>
#include <dftracer/utils/duql/string_literal.h>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Render-only: Field/F/Expr and Expr::to_string() are header-only and pull
// neither the parser nor simdjson, so a plugin links nothing. Expr::build() has
// a deduced return type and is defined in query.h, so only consumers that
// include query.h can call it (and only they pull the parser).
namespace dftracer::utils::duql {

namespace detail {

/// Turn a scalar into a LiteralNode, mirroring the parser's typing rules:
/// non-negative integers become uint64_t, negatives int64_t.
inline LiteralNode literal(bool v) { return LiteralNode{v}; }
inline LiteralNode literal(const char* v) {
    return LiteralNode{std::string(v)};
}
inline LiteralNode literal(std::string_view v) {
    return LiteralNode{std::string(v)};
}
inline LiteralNode literal(const std::string& v) { return LiteralNode{v}; }

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
    explicit Expr(QueryNodePtr node) : node_(std::move(node)) {}
    Expr(Expr&&) = default;
    Expr& operator=(Expr&&) = default;
    Expr(const Expr&) = delete;
    Expr& operator=(const Expr&) = delete;

    /// Serialize to the canonical DSL string.
    std::string to_string() const { return duql::to_string(*node_); }

    /// Borrow the underlying node.
    const QueryNode& node() const { return *node_; }

    /// Relinquish ownership of the underlying node.
    QueryNodePtr release() { return std::move(node_); }

    /// Build an executable Query (returns expected<Query, DuqlError>). Defined
    /// in query.h, so callers must include it; a plugin renders with
    /// to_string() instead and never pulls the parser.
    auto build() const;

   private:
    QueryNodePtr node_;
};

/// field op value with an explicit CompareOp and prebuilt literal.
inline Expr field_cmp(std::string_view name, CompareOp op, LiteralNode value) {
    return Expr(make_node(CompareNode{field_node(name), op, std::move(value)}));
}

#define DFTRACER_UTILS_DUQL_DEFINE_CMP(fn, op_enum)                \
    template <typename T>                                          \
    inline Expr fn(std::string_view name, T&& value) {             \
        return field_cmp(name, op_enum,                            \
                         detail::literal(std::forward<T>(value))); \
    }

DFTRACER_UTILS_DUQL_DEFINE_CMP(field_eq, CompareOp::EQ)
DFTRACER_UTILS_DUQL_DEFINE_CMP(field_ne, CompareOp::NE)
DFTRACER_UTILS_DUQL_DEFINE_CMP(field_gt, CompareOp::GT)
DFTRACER_UTILS_DUQL_DEFINE_CMP(field_lt, CompareOp::LT)
DFTRACER_UTILS_DUQL_DEFINE_CMP(field_ge, CompareOp::GE)
DFTRACER_UTILS_DUQL_DEFINE_CMP(field_le, CompareOp::LE)

#undef DFTRACER_UTILS_DUQL_DEFINE_CMP

/// field in [values].
inline Expr field_in(std::string_view name,
                     const std::vector<std::int64_t>& values) {
    ArrayNode arr;
    arr.elements.reserve(values.size());
    for (std::int64_t v : values) arr.elements.push_back(detail::literal(v));
    return Expr(make_node(InNode{field_node(name), std::move(arr)}));
}

inline Expr field_in(std::string_view name,
                     const std::vector<std::string>& values) {
    ArrayNode arr;
    arr.elements.reserve(values.size());
    for (const auto& v : values) arr.elements.push_back(LiteralNode{v});
    return Expr(make_node(InNode{field_node(name), std::move(arr)}));
}

/// field not in [values].
inline Expr field_not_in(std::string_view name,
                         const std::vector<std::int64_t>& values) {
    ArrayNode arr;
    arr.elements.reserve(values.size());
    for (std::int64_t v : values) arr.elements.push_back(detail::literal(v));
    return Expr(make_node(NotInNode{field_node(name), std::move(arr)}));
}

inline Expr field_not_in(std::string_view name,
                         const std::vector<std::string>& values) {
    ArrayNode arr;
    arr.elements.reserve(values.size());
    for (const auto& v : values) arr.elements.push_back(LiteralNode{v});
    return Expr(make_node(NotInNode{field_node(name), std::move(arr)}));
}

/// field like/ilike/~/~*/icontains pattern. The compiled matcher is filled in
/// by Expr::build() (or the C ABI wrap), not here.
inline Expr field_match(std::string_view name, MatchOp op,
                        std::string_view pattern, bool negated = false) {
    MatchNode node;
    node.field = field_node(name);
    node.op = op;
    node.pattern = std::string(pattern);
    node.negated = negated;
    return Expr(make_node(std::move(node)));
}

/// a and b.
inline Expr all_of(Expr a, Expr b) {
    return Expr(make_node(AndNode{a.release(), b.release()}));
}

/// a or b.
inline Expr any_of(Expr a, Expr b) {
    return Expr(make_node(OrNode{a.release(), b.release()}));
}

/// not a.
inline Expr negate(Expr a) { return Expr(make_node(NotNode{a.release()})); }

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
    explicit Field(std::string_view name) : name_(name) {}
    /// The same field as `any(<name>)`: a leaf on it holds when any element
    /// of the array holds.
    Field any() const { return Field("any(" + name_ + ")"); }

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

    Expr in(const std::vector<std::int64_t>& values) const {
        return field_in(name_, values);
    }
    Expr in(const std::vector<std::string>& values) const {
        return field_in(name_, values);
    }
    Expr not_in(const std::vector<std::int64_t>& values) const {
        return field_not_in(name_, values);
    }
    Expr not_in(const std::vector<std::string>& values) const {
        return field_not_in(name_, values);
    }

    Expr like(std::string_view pattern) const {
        return field_match(name_, MatchOp::LIKE, pattern);
    }
    Expr ilike(std::string_view pattern) const {
        return field_match(name_, MatchOp::ILIKE, pattern);
    }
    Expr regex(std::string_view pattern) const {
        return field_match(name_, MatchOp::REGEX, pattern);
    }
    Expr iregex(std::string_view pattern) const {
        return field_match(name_, MatchOp::IREGEX, pattern);
    }
    Expr contains(std::string_view substring) const {
        return field_match(name_, MatchOp::ICONTAINS, substring);
    }

   private:
    std::string name_;
};

/// Call-form field shorthand matching Python's `F("args.level")`:
/// `F("dur") < 25` is exactly `Field("dur") < 25`.
struct FieldFactory {
    Field operator()(std::string_view name) const { return Field(name); }
};
inline constexpr FieldFactory F{};

namespace detail {

inline std::string string_literal(std::string_view v) {
    return quote_string(v);
}

inline std::string number_text(double v) {
    if (!std::isfinite(v))
        throw std::invalid_argument("duql: a number must be finite");
    std::string s = double_text(v);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

}  // namespace detail

class Pipe;

/// A duql expression under construction, held as duql text. Every builder
/// call returns a new value; nothing runs until a Pipe is executed.
/// Converts implicitly from a literal: `c("dur") > 5`, `c("name") == "read"`.
/// A number that is not finite throws std::invalid_argument.
class Col {
   public:
    Col(std::nullptr_t) : text_("null") {}
    Col(bool v) : text_(v ? "true" : "false") {}
    template <typename I,
              std::enable_if_t<
                  std::is_integral_v<I> && !std::is_same_v<I, bool>, int> = 0>
    Col(I v) : text_(std::to_string(v)) {}
    template <typename D,
              std::enable_if_t<std::is_floating_point_v<D>, int> = 0>
    Col(D v) : text_(detail::number_text(static_cast<double>(v))) {}
    Col(const char* v) : text_(detail::string_literal(v)) {}
    Col(std::string_view v) : text_(detail::string_literal(v)) {}
    Col(const std::string& v) : text_(detail::string_literal(v)) {}

    /// A value from its duql text, unchecked; the parser checks it.
    static Col from_text(std::string text) {
        Col c(nullptr);
        c.text_ = std::move(text);
        return c;
    }

    const std::string& raw() const { return text_; }

    /// `path[index]`: `index` is an int, `param("n")` or any expression.
    /// Throws std::invalid_argument unless this is a field path; an index
    /// follows a field path.
    Col operator[](const Col& index) const {
        if (!is_path())
            throw std::invalid_argument("an index follows a field path");
        return from_text(text_ + "[" + index.text_ + "]");
    }

    Col is_in(const std::vector<Col>& values) const;
    Col not_in(const std::vector<Col>& values) const;
    /// `x in $p`: `list` is `param("p")`, bound to a list.
    Col is_in(const Col& list) const {
        return wrap(text_ + " in " + list.text_);
    }
    Col not_in(const Col& list) const {
        return wrap(text_ + " not in " + list.text_);
    }
    Col is_in(const Pipe& subquery) const;
    Col not_in(const Pipe& subquery) const;
    Col between(const Col& low, const Col& high) const {
        return wrap(text_ + " between " + low.text_ + " and " + high.text_);
    }
    Col not_between(const Col& low, const Col& high) const {
        return wrap(text_ + " not between " + low.text_ + " and " + high.text_);
    }
    /// A pattern is a string or `param("p")` bound to a string.
    Col like(const Col& pattern,
             std::optional<std::string_view> escape = std::nullopt) const {
        return match("like", pattern, escape);
    }
    Col ilike(const Col& pattern,
              std::optional<std::string_view> escape = std::nullopt) const {
        return match("ilike", pattern, escape);
    }
    Col not_like(const Col& pattern,
                 std::optional<std::string_view> escape = std::nullopt) const {
        return match("not like", pattern, escape);
    }
    Col not_ilike(const Col& pattern,
                  std::optional<std::string_view> escape = std::nullopt) const {
        return match("not ilike", pattern, escape);
    }
    Col regex(const Col& pattern) const { return binary("~", pattern); }
    Col iregex(const Col& pattern) const { return binary("~*", pattern); }
    Col not_regex(const Col& pattern) const { return binary("!~", pattern); }
    Col not_iregex(const Col& pattern) const { return binary("!~*", pattern); }
    /// `<this> over n rows`, a rows frame on a window call.
    Col over_rows(std::int64_t n) const {
        return from_text(text_ + " over " + std::to_string(n) + " rows");
    }
    /// `<this> over <width>`, a range frame on a window call; `width` is a
    /// number, `duration(...)` or `param(...)`.
    Col over(const Col& width) const {
        return from_text(text_ + " over " + width.text_);
    }
    Col is_null() const { return wrap(text_ + " is null"); }
    Col is_not_null() const { return wrap(text_ + " is not null"); }
    Col is_missing() const { return wrap(text_ + " is missing"); }
    Col is_not_missing() const { return wrap(text_ + " is not missing"); }
    Col coalesce(const Col& other) const { return binary("??", other); }
    /// The legacy case-insensitive substring test `"text" in path`, or
    /// `"text" in any(path)`; this value must be a path.
    Col icontains(std::string_view text, bool any = false) const {
        return contains(text, any, false);
    }
    Col not_icontains(std::string_view text, bool any = false) const {
        return contains(text, any, true);
    }
    /// `this -> rowset.path`, or `this -> rowset(key).path`.
    Col ref(std::string_view rowset, std::string_view path,
            std::string_view key = {}) const {
        std::string t = "(" + text_ + " -> " + std::string(rowset);
        if (!key.empty()) t += "(" + std::string(key) + ")";
        return from_text(t + "." + std::string(path) + ")");
    }

    Col binary(std::string_view op, const Col& right) const {
        return wrap(text_ + " " + std::string(op) + " " + right.text_);
    }

   private:
    static Col wrap(const std::string& t) { return from_text("(" + t + ")"); }
    Col match(std::string_view op, const Col& pattern,
              std::optional<std::string_view> escape) const {
        std::string t = text_ + " " + std::string(op) + " " + pattern.text_;
        if (escape) t += " escape " + detail::string_literal(*escape);
        return wrap(t);
    }
    Col contains(std::string_view text, bool any, bool negated) const {
        std::string t = detail::string_literal(text);
        t += negated ? " not in " : " in ";
        t += any ? "any(" + text_ + ")" : text_;
        return wrap(t);
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

    std::string text_;
};

/// A record path as written in duql: `a.b[0]`, `.x`, `^.x`, backtick keys.
inline Col c(std::string_view path) {
    return Col::from_text(std::string(path));
}
inline Col lit(const Col& v) { return v; }
inline Col param(std::string_view name) {
    return Col::from_text("$" + std::string(name));
}
/// `amount` followed by a unit: ns, us, ms, s, m or h.
template <typename N, std::enable_if_t<std::is_arithmetic_v<N>, int> = 0>
Col duration(N amount, std::string_view unit) {
    std::string t = std::is_integral_v<N>
                        ? std::to_string(amount)
                        : detail::number_text(static_cast<double>(amount));
    return Col::from_text(t + std::string(unit));
}

/// A named value: a `derive`, `agg` or `group` output, or a named argument.
struct Named {
    std::string name;
    Col value;
};

inline std::string join(const std::vector<Col>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out += ", ";
        out += items[i].raw();
    }
    return out;
}

/// `name(args, named = value, ...)`; `name` may be namespaced (`ns.f`).
inline Col fn(std::string_view name, const std::vector<Col>& args = {},
              const std::vector<Named>& named = {}) {
    std::string t = std::string(name) + "(" + join(args);
    for (std::size_t i = 0; i < named.size(); ++i) {
        if (i || !args.empty()) t += ", ";
        t += named[i].name + " = " + named[i].value.raw();
    }
    return Col::from_text(t + ")");
}
inline Col tup(const std::vector<Col>& items) {
    return Col::from_text("(" + join(items) + ")");
}
inline Col list(const std::vector<Col>& items) {
    return Col::from_text("[" + join(items) + "]");
}
/// `case { cond => value, ..., else => otherwise }`; a missing `otherwise`
/// is null.
inline Col case_(const std::vector<std::pair<Col, Col>>& pairs,
                 const std::optional<Col>& otherwise = std::nullopt) {
    std::vector<Col> args;
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
    return Col::from_text("(not " + a.raw() + ")");
}
/// Negation; in a sort key it sorts descending, as `-x` does in the text.
inline Col operator-(const Col& a) {
    return Col::from_text("-(" + a.raw() + ")");
}

/// A `select`, `group`, `window` or `session` item: a path (from a string),
/// an expression, or `name = expression`.
struct Item {
    Item(const char* path) : value(c(path)) {}
    Item(std::string_view path) : value(c(path)) {}
    Item(const std::string& path) : value(c(path)) {}
    Item(Col v) : value(std::move(v)) {}
    Item(std::string n, Col v) : value(std::move(v)), name(std::move(n)) {}
    Col value;
    std::string name;
};

enum class Nulls : std::uint8_t { DEFAULT, FIRST, LAST };

/// A sort key: a path (from a string) or an expression, ascending; `-x`
/// sorts descending.
struct SortKey {
    SortKey(const char* path) : value(c(path)) {}
    SortKey(const std::string& path) : value(c(path)) {}
    SortKey(Col v, Nulls n = Nulls::DEFAULT) : value(std::move(v)), nulls(n) {}
    Col value;
    Nulls nulls = Nulls::DEFAULT;
};

/// A lookup key: `left`, or `left == right` (`right` the row set's side).
struct JoinKey {
    JoinKey(const char* path) : left(c(path)) {}
    JoinKey(const std::string& path) : left(c(path)) {}
    JoinKey(Col l) : left(std::move(l)) {}
    JoinKey(Col l, Col r) : left(std::move(l)), right(std::move(r)) {}
    Col left;
    std::optional<Col> right;
};

enum class Asof : std::uint8_t { BACKWARD, FORWARD, NEAREST };

/// How `lookup` treats rows: LEFT keeps every row, INNER drops rows with no
/// match, ANTI keeps only rows with no match (and takes no `into`).
enum class LookupHow : std::uint8_t { LEFT, INNER, ANTI };

/// A duql query under construction, held as duql text with its bound
/// parameters. Stage methods return a new Pipe. A Pipe with no source reads
/// the data of the View that runs it.
class Pipe {
   public:
    Pipe() = default;

    /// The builder's duql text, not canonical.
    std::string raw() const;
    /// The canonical duql text, the engine's `duql MAJOR.MINOR` first. Throws
    /// std::invalid_argument with the parser's message when the text does not
    /// parse (a malformed path or call). Defined in the duql library, not in
    /// this header.
    std::string text() const;
    /// The bound parameters, by name without the `$`.
    const std::vector<std::pair<std::string, ParamValue>>& bound() const {
        return params_;
    }
    /// Binds `$name` to a scalar, or to a list for `x in $name`.
    Pipe bind(std::string name, ParamValue value) const {
        Pipe p = *this;
        for (auto& kv : p.params_)
            if (kv.first == name) {
                kv.second = std::move(value);
                return p;
            }
        p.params_.emplace_back(std::move(name), std::move(value));
        return p;
    }

    Pipe let(std::string_view name, const Pipe& pipeline) const {
        return decl("let " + std::string(name) + " = " +
                    pipeline.inline_text());
    }
    Pipe define(std::string_view name, const std::vector<std::string>& params,
                const Col& body) const {
        return define_text(name, params, body.raw());
    }
    /// `def name(params) = stages;`: a pipeline macro. `body` must not start
    /// with a source.
    Pipe define(std::string_view name, const std::vector<std::string>& params,
                const Pipe& body) const {
        return define_text(name, params, body.inline_text());
    }
    /// A stage that calls the pipeline macro `name` with `args`.
    Pipe use(std::string_view name, const std::vector<Col>& args = {}) const {
        std::string t = std::string(name) + "(";
        for (std::size_t i = 0; i < args.size(); ++i)
            t += (i ? ", " : "") + args[i].raw();
        return stage(t + ")");
    }
    Pipe define_text(std::string_view name,
                     const std::vector<std::string>& params,
                     const std::string& body) const {
        std::string t = "def " + std::string(name);
        if (!params.empty()) {
            t += "(";
            for (std::size_t i = 0; i < params.size(); ++i)
                t += (i ? ", " : "") + params[i];
            t += ")";
        }
        return decl(t + " = " + body);
    }

    Pipe where(const Col& condition) const {
        return stage("where " + condition.raw());
    }
    Pipe where(const Expr& condition) const {
        return stage("where (" + condition.to_string() + ")");
    }
    Pipe derive(const std::vector<Named>& fields) const {
        return stage("derive " + assigns(fields));
    }
    Pipe select(const std::vector<Item>& items) const {
        return stage("select " + items_text(items));
    }
    Pipe drop(const std::vector<std::string>& paths) const {
        return stage("drop " + names(paths));
    }
    /// `rename new = old, ...`.
    Pipe rename(
        const std::vector<std::pair<std::string, std::string>>& pairs) const {
        std::string t = "rename ";
        for (std::size_t i = 0; i < pairs.size(); ++i)
            t += (i ? ", " : "") + pairs[i].first + " = " + pairs[i].second;
        return stage(t);
    }
    Pipe distinct(const std::vector<Item>& keys = {}) const {
        return stage(keys.empty() ? "distinct"
                                  : "distinct " + items_text(keys));
    }
    Pipe group(const std::vector<Item>& keys,
               const std::vector<Named>& aggregates) const {
        std::string t = "group ";
        if (!keys.empty()) t += items_text(keys) + " ";
        return stage(t + block(aggregates));
    }
    Pipe agg(const std::vector<Named>& aggregates) const {
        return stage("agg " + block(aggregates));
    }
    Pipe window(const std::vector<Item>& partition,
                const std::vector<SortKey>& order,
                const std::vector<Named>& fields) const {
        std::string t = "window ";
        if (!partition.empty()) t += items_text(partition) + " ";
        if (!order.empty()) t += "sort " + sort_keys(order) + " ";
        return stage(t + block(fields));
    }
    /// `labels` pairs with `values`; a non-empty label names that column.
    Pipe pivot(const Col& key, const std::vector<Col>& values,
               const std::vector<Named>& aggregates,
               const std::vector<std::string>& labels = {}) const {
        std::string t = "pivot " + key.raw();
        if (!values.empty()) {
            t += " in [";
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (i) t += ", ";
                t += values[i].raw();
                if (i < labels.size() && !labels[i].empty())
                    t += " as " + labels[i];
            }
            t += "]";
        }
        return stage(t + " " + block(aggregates));
    }
    Pipe unpivot(const std::vector<std::string>& paths,
                 std::string_view key_name, std::string_view value_name) const {
        return stage("unpivot " + names(paths) + " as " +
                     std::string(key_name) + ", " + std::string(value_name));
    }
    Pipe sort(const std::vector<SortKey>& keys) const {
        return stage("sort " + sort_keys(keys));
    }
    Pipe take(const Col& count, const std::vector<Col>& by = {},
              const std::vector<SortKey>& order = {}) const {
        std::string t = "take " + count.raw();
        if (!by.empty()) t += " by " + join(by);
        if (!order.empty()) t += " sort " + sort_keys(order);
        return stage(t);
    }
    /// `take first..last`: rows `first` to `last`, 1-based and inclusive.
    Pipe take_range(const Col& first, const Col& last) const {
        return stage("take " + first.raw() + ".." + last.raw());
    }
    Pipe skip(const Col& count) const { return stage("skip " + count.raw()); }
    Pipe sample(const Col& amount, bool percent = false,
                std::optional<Col> seed = std::nullopt) const {
        std::string t = "sample " + amount.raw() + (percent ? "%" : "");
        if (seed) t += " seed " + seed->raw();
        return stage(t);
    }
    Pipe expand(std::string_view path, std::string_view as = {},
                std::string_view with_index = {},
                bool keep_empty = false) const {
        std::string t = "expand " + std::string(path);
        if (!as.empty()) t += " as " + std::string(as);
        if (!with_index.empty()) t += " with_index " + std::string(with_index);
        if (keep_empty) t += " keep_empty";
        return stage(t);
    }
    /// `parse col ~ pattern`; `pattern` is a string or `param("p")` bound to a
    /// string, and its named groups become columns.
    Pipe parse(const Col& col, const Col& pattern) const {
        return stage("parse " + col.raw() + " ~ " + pattern.raw());
    }
    Pipe lookup(std::string_view rowset, const std::vector<JoinKey>& on,
                std::string_view into = {},
                LookupHow how = LookupHow::LEFT) const {
        return lookup_text(std::string(rowset), on, into, how);
    }
    /// `side` needs a source; it prints as `(from ...)`.
    Pipe lookup(const Pipe& side, const std::vector<JoinKey>& on,
                std::string_view into = {},
                LookupHow how = LookupHow::LEFT) const {
        return lookup_text("(" + side.inline_text() + ")", on, into, how);
    }
    Pipe lookup_asof(std::string_view rowset, const std::vector<JoinKey>& on,
                     const JoinKey& time, Asof direction = Asof::BACKWARD,
                     std::optional<Col> within = std::nullopt) const {
        return asof_text(std::string(rowset), on, time, direction, within);
    }
    Pipe lookup_asof(const Pipe& side, const std::vector<JoinKey>& on,
                     const JoinKey& time, Asof direction = Asof::BACKWARD,
                     std::optional<Col> within = std::nullopt) const {
        return asof_text("(" + side.inline_text() + ")", on, time, direction,
                         within);
    }
    Pipe lookup_overlap(std::string_view rowset, const std::vector<JoinKey>& on,
                        std::string_view into = {}) const {
        return overlap_text(std::string(rowset), on, into);
    }
    Pipe lookup_overlap(const Pipe& side, const std::vector<JoinKey>& on,
                        std::string_view into = {}) const {
        return overlap_text("(" + side.inline_text() + ")", on, into);
    }
    /// `union (other)`; `other` needs a source.
    Pipe union_with(const Pipe& other) const {
        return stage("union (" + other.inline_text() + ")");
    }
    /// `union name`: the rows of the row set or `let` `name`.
    Pipe union_with(std::string_view name) const {
        return stage("union " + std::string(name));
    }
    Pipe call(const Col& call) const { return stage("call " + call.raw()); }
    /// `time_range low .. high`; an absent bound leaves that side open.
    Pipe time_range(std::optional<Col> low, std::optional<Col> high,
                    bool overlap = false) const {
        if (!low && !high)
            throw std::invalid_argument(
                "duql: time_range needs a low or a high bound");
        std::string t = "time_range ";
        if (low) t += low->raw() + " ";
        t += "..";
        if (high) t += " " + high->raw();
        return stage(t + (overlap ? " overlap" : ""));
    }
    Pipe call_tree() const { return stage("call_tree"); }
    /// `bucket width [every e] [at t] [fill [mode] [from low to high]] [as
    /// name]`; `mode` is "zero", "forward" or "linear", and a mode or range
    /// needs `fill`.
    Pipe bucket(const Col& width, bool fill = false, std::string_view as = {},
                std::string_view mode = "zero",
                std::optional<Col> low = std::nullopt,
                std::optional<Col> high = std::nullopt,
                std::optional<Col> every = std::nullopt,
                std::optional<Col> at = std::nullopt) const {
        if (mode != "zero" && mode != "forward" && mode != "linear")
            throw std::invalid_argument(
                "duql: bucket mode is zero, forward or linear");
        if (!fill && (mode != "zero" || low || high))
            throw std::invalid_argument(
                "duql: a bucket mode or range needs fill");
        if (low.has_value() != high.has_value())
            throw std::invalid_argument(
                "duql: a bucket range needs both low and high");
        std::string t = "bucket " + width.raw();
        if (every) t += " every " + every->raw();
        if (at) t += " at " + at->raw();
        if (fill) t += " fill";
        if (mode != "zero") t += " " + std::string(mode);
        if (low) t += " from " + low->raw() + " to " + high->raw();
        if (!as.empty()) t += " as " + std::string(as);
        return stage(t);
    }
    Pipe session(const std::vector<Item>& keys, const Col& gap,
                 std::optional<Col> max = std::nullopt,
                 std::string_view as = {}) const {
        std::string t = "session ";
        if (!keys.empty()) t += items_text(keys) + " ";
        t += "gap " + gap.raw();
        if (max) t += " max " + max->raw();
        if (!as.empty()) t += " as " + std::string(as);
        return stage(t);
    }

    /// The pipeline on one line, stages joined by ` | `, without declarations.
    std::string inline_text() const {
        std::string out = from_;
        for (const auto& s : stages_) {
            if (!out.empty()) out += " | ";
            out += s;
        }
        return out;
    }

   private:
    friend Pipe source(std::string_view file);
    friend Pipe sources(const std::vector<std::string>& files);
    friend Pipe rowset(std::string_view name);
    friend Pipe source_param(std::string_view name);

    Pipe stage(std::string text) const {
        Pipe p = *this;
        p.stages_.push_back(std::move(text));
        return p;
    }
    Pipe decl(std::string text) const {
        Pipe p = *this;
        p.decls_.push_back(std::move(text));
        return p;
    }
    static std::string names(const std::vector<std::string>& list) {
        std::string out;
        for (std::size_t i = 0; i < list.size(); ++i)
            out += (i ? ", " : "") + list[i];
        return out;
    }
    static std::string assigns(const std::vector<Named>& fields) {
        std::string out;
        for (std::size_t i = 0; i < fields.size(); ++i)
            out += (i ? ", " : "") + fields[i].name + " = " +
                   fields[i].value.raw();
        return out;
    }
    static std::string block(const std::vector<Named>& fields) {
        return fields.empty() ? "{ }" : "{ " + assigns(fields) + " }";
    }
    static std::string items_text(const std::vector<Item>& items) {
        std::string out;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i) out += ", ";
            if (!items[i].name.empty()) out += items[i].name + " = ";
            out += items[i].value.raw();
        }
        return out;
    }
    static std::string sort_keys(const std::vector<SortKey>& keys) {
        std::string out;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (i) out += ", ";
            out += keys[i].value.raw();
            if (keys[i].nulls == Nulls::FIRST) out += " nulls first";
            if (keys[i].nulls == Nulls::LAST) out += " nulls last";
        }
        return out;
    }
    static std::string key_text(const JoinKey& k) {
        return k.right ? k.left.raw() + " == " + k.right->raw() : k.left.raw();
    }
    Pipe lookup_text(const std::string& side, const std::vector<JoinKey>& on,
                     std::string_view into, LookupHow how) const {
        if (how == LookupHow::ANTI && !into.empty())
            throw std::invalid_argument(
                "duql: an anti lookup cannot take into");
        std::string t = join_keys(side, on);
        if (how == LookupHow::INNER) t += " inner";
        if (how == LookupHow::ANTI) t += " anti";
        if (!into.empty()) t += " into " + std::string(into);
        return stage(t);
    }
    Pipe asof_text(const std::string& side, const std::vector<JoinKey>& on,
                   const JoinKey& time, Asof direction,
                   const std::optional<Col>& within) const {
        std::string t = join_keys(side, on) + " asof " + key_text(time);
        if (direction == Asof::FORWARD) t += " forward";
        if (direction == Asof::NEAREST) t += " nearest";
        if (within) t += " within " + within->raw();
        return stage(t);
    }
    Pipe overlap_text(const std::string& side, const std::vector<JoinKey>& on,
                      std::string_view into) const {
        std::string t = join_keys(side, on) + " overlap";
        if (!into.empty()) t += " into " + std::string(into);
        return stage(t);
    }
    static std::string join_keys(const std::string& rowset,
                                 const std::vector<JoinKey>& on) {
        std::string t = "lookup " + rowset + " on ";
        for (std::size_t i = 0; i < on.size(); ++i)
            t += (i ? ", " : "") + key_text(on[i]);
        return t;
    }

    std::string from_;
    std::vector<std::string> decls_;
    std::vector<std::string> stages_;
    std::vector<std::pair<std::string, ParamValue>> params_;
};

/// A pipeline over the trace file `file`: `from "file"`.
inline Pipe source(std::string_view file) {
    Pipe p;
    p.from_ = "from " + detail::string_literal(file);
    return p;
}
/// A pipeline over several trace files.
inline Pipe sources(const std::vector<std::string>& files) {
    Pipe p;
    p.from_ = "from ";
    for (std::size_t i = 0; i < files.size(); ++i)
        p.from_ += (i ? ", " : "") + detail::string_literal(files[i]);
    return p;
}
/// A pipeline over the row set or `let` `name` (`all`, `data`, ...).
inline Pipe rowset(std::string_view name) {
    Pipe p;
    p.from_ = "from " + std::string(name);
    return p;
}
/// A pipeline over the file bound to parameter `name`: `from $name`.
inline Pipe source_param(std::string_view name) {
    Pipe p;
    p.from_ = "from $" + std::string(name);
    return p;
}

/// A scalar sub-query: the only cell of `pipeline`, which needs a source.
inline Col sub(const Pipe& pipeline) {
    return Col::from_text("(" + pipeline.inline_text() + ")");
}

inline Col Col::is_in(const std::vector<Col>& values) const {
    return wrap(text_ + " in [" + join(values) + "]");
}
inline Col Col::not_in(const std::vector<Col>& values) const {
    return wrap(text_ + " not in [" + join(values) + "]");
}
inline Col Col::is_in(const Pipe& subquery) const {
    return wrap(text_ + " in (" + subquery.inline_text() + ")");
}
inline Col Col::not_in(const Pipe& subquery) const {
    return wrap(text_ + " not in (" + subquery.inline_text() + ")");
}

inline std::string Pipe::raw() const {
    std::string out;
    for (const auto& d : decls_) out += d + ";\n";
    return out + inline_text();
}

/// The member text of a duql source, in the order added. Each method returns
/// a new Source.
class Source {
   public:
    /// `name = pipeline`. Throws std::invalid_argument when `pipeline` starts
    /// with a source (`from ...`): a source member reads the data, not a file.
    Source rowset(std::string_view name, const Pipe& pipeline) const {
        const std::string body = pipeline.inline_text();
        if (body.rfind("from ", 0) == 0)
            throw std::invalid_argument(
                "a source row set cannot start with from");
        return add(std::string(name) + " = " + body);
    }
    /// `def name(params) = body`: a column macro.
    Source define(std::string_view name, const std::vector<std::string>& params,
                  const Col& body) const {
        return add(Pipe().define(name, params, body));
    }
    /// `def name(params) = stages`: a pipeline macro; `body` must not start
    /// with a source.
    Source define(std::string_view name, const std::vector<std::string>& params,
                  const Pipe& body) const {
        return add(Pipe().define(name, params, body));
    }
    /// `def name = true` or `def name = false`.
    Source flag(std::string_view name, bool value) const {
        return add("def " + std::string(name) +
                   (value ? " = true" : " = false"));
    }
    /// The members as canonical text joined by `;\n`, the form merge_source
    /// stores. Throws std::invalid_argument with the parser's message when a
    /// member does not parse. Defined in the duql library.
    std::string text() const;

   private:
    Source add(const Pipe& decl) const {
        std::string t = decl.raw();
        t.resize(t.size() - 2);
        return add(std::move(t));
    }
    Source add(std::string member) const {
        Source s = *this;
        s.members_.push_back(std::move(member));
        return s;
    }
    std::vector<std::string> members_;
};

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_BUILDER_H
