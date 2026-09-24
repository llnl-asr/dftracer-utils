#ifndef DFTRACER_UTILS_QUERY_BUILDER_H
#define DFTRACER_UTILS_QUERY_BUILDER_H

#include <dftracer/utils/query/ast.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Render-only: Field/F/Expr and Expr::to_string() are header-only and pull
// neither the parser nor simdjson, so a plugin links nothing. Expr::build() has
// a deduced return type and is defined in query.h, so only consumers that
// include query.h can call it (and only they pull the parser).
namespace dftracer::utils::query {

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
    std::string to_string() const { return query::to_string(*node_); }

    /// Borrow the underlying node.
    const QueryNode& node() const { return *node_; }

    /// Relinquish ownership of the underlying node.
    QueryNodePtr release() { return std::move(node_); }

    /// Build an executable Query (returns expected<Query, QueryError>). Defined
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

#define DFTRACER_UTILS_QUERY_DEFINE_CMP(fn, op_enum)               \
    template <typename T>                                          \
    inline Expr fn(std::string_view name, T&& value) {             \
        return field_cmp(name, op_enum,                            \
                         detail::literal(std::forward<T>(value))); \
    }

DFTRACER_UTILS_QUERY_DEFINE_CMP(field_eq, CompareOp::EQ)
DFTRACER_UTILS_QUERY_DEFINE_CMP(field_ne, CompareOp::NE)
DFTRACER_UTILS_QUERY_DEFINE_CMP(field_gt, CompareOp::GT)
DFTRACER_UTILS_QUERY_DEFINE_CMP(field_lt, CompareOp::LT)
DFTRACER_UTILS_QUERY_DEFINE_CMP(field_ge, CompareOp::GE)
DFTRACER_UTILS_QUERY_DEFINE_CMP(field_le, CompareOp::LE)

#undef DFTRACER_UTILS_QUERY_DEFINE_CMP

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
/// with `&&`, `||`, `!`. Mirrors the Python `dftracer.utils.query.Field` API.
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

/// A resolved-hash field: `resolved("name")` targets `resolved.name`, matching
/// the Python `resolved()` helper.
inline Field resolved(std::string_view name) {
    return Field("resolved." + std::string(name));
}

/// Call-form field shorthand matching Python's `F("args.level")`:
/// `F("dur") < 25` is exactly `Field("dur") < 25`.
struct FieldFactory {
    Field operator()(std::string_view name) const { return Field(name); }
};
inline constexpr FieldFactory F{};

}  // namespace dftracer::utils::query

#endif  // DFTRACER_UTILS_QUERY_BUILDER_H
