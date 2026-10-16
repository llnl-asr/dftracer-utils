#ifndef DFTRACER_UTILS_DUQL_TERM_H
#define DFTRACER_UTILS_DUQL_TERM_H

#include <dftracer/utils/duql/ast.h>
#include <dftracer/utils/duql/syntax/tree.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dftracer::utils::duql {

enum class Fn : std::uint8_t {
    Exists,
    Coalesce,
    If,
    Case,
    Abs,
    Floor,
    Ceil,
    Round,
    Min,
    Max,
    Log,
    Exp,
    Pow,
    Len,
    Concat,
    Lower,
    Upper,
    Trim,
    StartsWith,
    EndsWith,
    Contains,
    Substr,
    Replace,
    First,
    Last,
    Sum,
    Json,
    Type,
    Int,
    Float,
    String,
    Extract,
    Slice,
    Flatten,
    Keys,
    Values,
    ParseJson,
    Split,
    RegexReplace,
    DatePart,
    FormatTime,
    IndexOf,
    Sort,
    Unique,
    Join,
};

struct FnInfo {
    Fn fn;
    std::string_view name;
    std::uint8_t min_args;
    std::uint8_t max_args;  ///< VARIADIC for no upper bound.
};

inline constexpr std::uint8_t VARIADIC = 0xFF;

/// Every built-in function, in Fn order.
inline constexpr FnInfo FUNCTIONS[] = {
    {Fn::Exists, "exists", 1, 1},
    {Fn::Coalesce, "coalesce", 1, VARIADIC},
    {Fn::If, "if", 3, 3},
    {Fn::Case, "case", 3, VARIADIC},
    {Fn::Abs, "abs", 1, 1},
    {Fn::Floor, "floor", 1, 1},
    {Fn::Ceil, "ceil", 1, 1},
    {Fn::Round, "round", 1, 2},
    {Fn::Min, "min", 1, VARIADIC},
    {Fn::Max, "max", 1, VARIADIC},
    {Fn::Log, "log", 1, 1},
    {Fn::Exp, "exp", 1, 1},
    {Fn::Pow, "pow", 2, 2},
    {Fn::Len, "len", 1, 1},
    {Fn::Concat, "concat", 1, VARIADIC},
    {Fn::Lower, "lower", 1, 1},
    {Fn::Upper, "upper", 1, 1},
    {Fn::Trim, "trim", 1, 1},
    {Fn::StartsWith, "starts_with", 2, 2},
    {Fn::EndsWith, "ends_with", 2, 2},
    {Fn::Contains, "contains", 2, 2},
    {Fn::Substr, "substr", 2, 3},
    {Fn::Replace, "replace", 3, 3},
    {Fn::First, "first", 1, 1},
    {Fn::Last, "last", 1, 1},
    {Fn::Sum, "sum", 1, 1},
    {Fn::Json, "json", 1, 1},
    {Fn::Type, "type", 1, 1},
    {Fn::Int, "int", 1, 1},
    {Fn::Float, "float", 1, 1},
    {Fn::String, "string", 1, 1},
    {Fn::Extract, "extract", 2, 3},
    {Fn::Slice, "slice", 2, 3},
    {Fn::Flatten, "flatten", 1, 1},
    {Fn::Keys, "keys", 1, 1},
    {Fn::Values, "values", 1, 1},
    {Fn::ParseJson, "parse_json", 1, 1},
    {Fn::Split, "split", 2, 2},
    {Fn::RegexReplace, "regex_replace", 3, 3},
    {Fn::DatePart, "date_part", 2, 2},
    {Fn::FormatTime, "format_time", 2, 2},
    {Fn::IndexOf, "index_of", 2, 2},
    {Fn::Sort, "sort", 1, 1},
    {Fn::Unique, "unique", 1, 1},
    {Fn::Join, "join", 2, 2},
};

inline const FnInfo& fn_info(Fn fn) {
    return FUNCTIONS[static_cast<std::size_t>(fn)];
}

struct Term;
using TermPtr = std::unique_ptr<const Term>;

struct TNull {};

/// A constant; parameters are bound to constants when a query is lowered.
struct TConst {
    std::variant<TNull, bool, std::int64_t, std::uint64_t, double, std::string>
        value;
};

/// Where a path starts: the record, the current element of a quantifier's
/// array (`.`), or the record around that element (`^.`).
enum class FieldRoot : std::uint8_t { Record, Element, Outer };

/// A path. For a record or outer path, `base` is the path up to the first
/// negative index, in the form readers and field maps use (`a.b`, `a[0]`);
/// from there the index of step `neg_at`, then every later step, is walked
/// from the value at `base`. An element path walks every step from the
/// element and has no base.
struct TField {
    std::string base;
    std::vector<syntax::PathStep> steps;
    std::size_t neg_at = 0;
    FieldRoot root = FieldRoot::Record;
};

enum class TermOp : std::uint8_t {
    Neg,
    Not,
    Add,
    Sub,
    Mul,
    Div,
    Idiv,
    Mod,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
    Coalesce,
};

struct TUnary {
    TermOp op;
    TermPtr operand;
};

struct TBinary {
    TermOp op;
    TermPtr left;
    TermPtr right;
};

struct TBetween {
    TermPtr subject;
    TermPtr low;
    TermPtr high;
    bool negated = false;
};

/// `is [not] null` or `is [not] missing`.
struct TIs {
    TermPtr subject;
    bool missing = false;
    bool negated = false;
};

struct TIn {
    TermPtr subject;
    std::vector<TermPtr> list;
    bool negated = false;
};

struct Substitution;

struct TCall {
    Fn fn;
    std::vector<TermPtr> args;
    /// EXTRACT, REGEX_REPLACE: the compiled regex of its second argument.
    std::shared_ptr<const CompiledPattern> pattern;
    /// REGEX_REPLACE: the third argument compiled against `pattern`.
    std::shared_ptr<const Substitution> substitution;
    /// DATE_PART, FORMAT_TIME: nanoseconds per unit of the time argument.
    /// The second argument stays the user's part name or format constant.
    std::int64_t ns_per_unit = 0;
    /// DATE_PART: the DatePart code of the second argument.
    std::int32_t part = 0;
};

/// `any(subject, cond)`, or `all(subject, cond)` when `all`: `cond` holds
/// for some (every) element of the array `subject`.
struct TQuant {
    bool all = false;
    TermPtr subject;
    TermPtr cond;
};

struct LookupTable;

/// The table a TLookup reads, bound after its side runs; null until then.
struct LookupSlot {
    std::shared_ptr<const LookupTable> table;
};

/// How a TLookup reads its side: IN tests that some row has the key, ARROW
/// reads a column of the row with the key, SCALAR is the side's only cell.
enum class LookupKind : std::uint8_t { In, Arrow, Scalar };

/// What a range-correlated lookup gives for the side rows in its range; NONE
/// for a lookup without a range. ROWS: IN tests that there is one, SCALAR
/// reads `column` of the only one. COUNT_VALUES counts the non-null values
/// of `column`, COUNT_IF its true ones; the rest aggregate `column`.
enum class RangeRead : std::uint8_t {
    None,
    Rows,
    Count,
    CountValues,
    CountIf,
    Sum,
    Min,
    Max,
    Mean,
};

/// A read of the row set `side` of the program. `keys` are this row's key
/// terms, matched against the side's columns `target` (IN: every column).
/// ARROW reads `column`; with `all`, every matching row's value as a list,
/// null when none.
///
/// A correlated sub-query is a side keyed by the enclosing row: the last
/// `correlated` of `keys` are the enclosing row's, matched against `target`. IN
/// compares the other keys with the side's values, and a row whose correlated
/// key has no match is FALSE. A keyed SCALAR reads `column` of the one row of
/// its key: none gives the row of side `empty` (the sub-query over no rows)
/// when it has one, else null, and several are an error.
///
/// With a `range`, the last `(low ? 1 : 0) + (high ? 1 : 0)` keys are the
/// enclosing row's bounds of the side's CORRELATED_RANGE column, lower first:
/// `low` is GT or GE, `high` LT or LE. They count in `correlated` and are not
/// in `target`.
struct TLookup {
    std::vector<TermPtr> keys;
    std::size_t side = 0;
    std::string name;
    std::vector<std::string> target;
    std::string column;
    std::size_t correlated = 0;
    std::optional<std::size_t> empty;
    std::shared_ptr<LookupSlot> slot;
    std::optional<TermOp> low;
    std::optional<TermOp> high;
    LookupKind kind = LookupKind::In;
    RangeRead range = RangeRead::None;
    bool negated = false;
    bool all = false;
    /// IN only: a top-level term of the scan filter, which reads the side's
    /// distinct rows from a table instead of a join.
    bool key_set = false;
};

/// Whether `l` reads its side through a join, not a collected table: an
/// `in` sub-query off the scan filter, or a sub-query correlated by `==`
/// keys alone.
inline bool joined(const TLookup& l) {
    return l.range == RangeRead::None &&
           ((l.kind == LookupKind::In && !l.key_set) ||
            (l.kind == LookupKind::Scalar && !l.keys.empty()));
}

struct TMatch {
    TermPtr subject;
    MatchOp op;
    std::string pattern;
    bool negated = false;
    std::optional<char> escape;
    std::shared_ptr<const CompiledPattern> compiled;
};

/// `array[index]`: the element of `array` at a computed index, 0-based and
/// from the end when negative; null out of range or when `array` is not one.
struct TIndex {
    TermPtr array;
    TermPtr index;
};

/// A list literal; a missing item is null.
struct TList {
    std::vector<TermPtr> items;
};

struct Term {
    std::variant<TConst, TField, TUnary, TBinary, TBetween, TIs, TIn, TCall,
                 TMatch, TQuant, TLookup, TIndex, TList>
        node;
};

/// The duql text of `t`, which parses back to the same term unless it holds
/// a TLookup.
std::string term_text(const Term& t);

/// Calls `fn` with `t` and every term under it.
template <class F>
void for_each_term(const Term& t, F&& fn) {
    fn(t);
    std::visit(
        [&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, TUnary>) {
                for_each_term(*n.operand, fn);
            } else if constexpr (std::is_same_v<T, TBinary>) {
                for_each_term(*n.left, fn);
                for_each_term(*n.right, fn);
            } else if constexpr (std::is_same_v<T, TBetween>) {
                for_each_term(*n.subject, fn);
                for_each_term(*n.low, fn);
                for_each_term(*n.high, fn);
            } else if constexpr (std::is_same_v<T, TIs> ||
                                 std::is_same_v<T, TMatch>) {
                for_each_term(*n.subject, fn);
            } else if constexpr (std::is_same_v<T, TIn>) {
                for_each_term(*n.subject, fn);
                for (const auto& e : n.list) for_each_term(*e, fn);
            } else if constexpr (std::is_same_v<T, TCall>) {
                for (const auto& a : n.args) for_each_term(*a, fn);
            } else if constexpr (std::is_same_v<T, TQuant>) {
                for_each_term(*n.subject, fn);
                for_each_term(*n.cond, fn);
            } else if constexpr (std::is_same_v<T, TLookup>) {
                for (const auto& k : n.keys) for_each_term(*k, fn);
            } else if constexpr (std::is_same_v<T, TIndex>) {
                for_each_term(*n.array, fn);
                for_each_term(*n.index, fn);
            } else if constexpr (std::is_same_v<T, TList>) {
                for (const auto& x : n.items) for_each_term(*x, fn);
            }
        },
        t.node);
}

/// Calls `fn` with every record or outer field under `t`.
template <class F>
void for_each_term_field(const Term& t, F&& fn) {
    for_each_term(t, [&fn](const Term& x) {
        if (const auto* f = std::get_if<TField>(&x.node);
            f && f->root != FieldRoot::Element)
            fn(*f);
    });
}

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_TERM_H
