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
    EXISTS,
    COALESCE,
    IF,
    CASE,
    ABS,
    FLOOR,
    CEIL,
    ROUND,
    MIN,
    MAX,
    LOG,
    EXP,
    POW,
    LEN,
    CONCAT,
    LOWER,
    UPPER,
    TRIM,
    STARTS_WITH,
    ENDS_WITH,
    CONTAINS,
    SUBSTR,
    REPLACE,
    FIRST,
    LAST,
    SUM,
    JSON,
    TYPE,
    INT,
    FLOAT,
    STRING,
    EXTRACT,
    SLICE,
    FLATTEN,
    KEYS,
    VALUES,
    PARSE_JSON,
    SPLIT,
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
    {Fn::EXISTS, "exists", 1, 1},
    {Fn::COALESCE, "coalesce", 1, VARIADIC},
    {Fn::IF, "if", 3, 3},
    {Fn::CASE, "case", 3, VARIADIC},
    {Fn::ABS, "abs", 1, 1},
    {Fn::FLOOR, "floor", 1, 1},
    {Fn::CEIL, "ceil", 1, 1},
    {Fn::ROUND, "round", 1, 2},
    {Fn::MIN, "min", 1, VARIADIC},
    {Fn::MAX, "max", 1, VARIADIC},
    {Fn::LOG, "log", 1, 1},
    {Fn::EXP, "exp", 1, 1},
    {Fn::POW, "pow", 2, 2},
    {Fn::LEN, "len", 1, 1},
    {Fn::CONCAT, "concat", 1, VARIADIC},
    {Fn::LOWER, "lower", 1, 1},
    {Fn::UPPER, "upper", 1, 1},
    {Fn::TRIM, "trim", 1, 1},
    {Fn::STARTS_WITH, "starts_with", 2, 2},
    {Fn::ENDS_WITH, "ends_with", 2, 2},
    {Fn::CONTAINS, "contains", 2, 2},
    {Fn::SUBSTR, "substr", 2, 3},
    {Fn::REPLACE, "replace", 3, 3},
    {Fn::FIRST, "first", 1, 1},
    {Fn::LAST, "last", 1, 1},
    {Fn::SUM, "sum", 1, 1},
    {Fn::JSON, "json", 1, 1},
    {Fn::TYPE, "type", 1, 1},
    {Fn::INT, "int", 1, 1},
    {Fn::FLOAT, "float", 1, 1},
    {Fn::STRING, "string", 1, 1},
    {Fn::EXTRACT, "extract", 2, 3},
    {Fn::SLICE, "slice", 2, 3},
    {Fn::FLATTEN, "flatten", 1, 1},
    {Fn::KEYS, "keys", 1, 1},
    {Fn::VALUES, "values", 1, 1},
    {Fn::PARSE_JSON, "parse_json", 1, 1},
    {Fn::SPLIT, "split", 2, 2},
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
enum class FieldRoot : std::uint8_t { RECORD, ELEMENT, OUTER };

/// A path. For a record or outer path, `base` is the path up to the first
/// negative index, in the form readers and field maps use (`a.b`, `a[0]`);
/// from there the index of step `neg_at`, then every later step, is walked
/// from the value at `base`. An element path walks every step from the
/// element and has no base.
struct TField {
    std::string base;
    std::vector<syntax::PathStep> steps;
    std::size_t neg_at = 0;
    FieldRoot root = FieldRoot::RECORD;
};

enum class TermOp : std::uint8_t {
    NEG,
    NOT,
    ADD,
    SUB,
    MUL,
    DIV,
    IDIV,
    MOD,
    EQ,
    NE,
    LT,
    LE,
    GT,
    GE,
    AND,
    OR,
    COALESCE,
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

struct TCall {
    Fn fn;
    std::vector<TermPtr> args;
    /// EXTRACT: the compiled regex of its second argument.
    std::shared_ptr<const CompiledPattern> pattern;
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
enum class LookupKind : std::uint8_t { IN, ARROW, SCALAR };

/// What a range-correlated lookup gives for the side rows in its range; NONE
/// for a lookup without a range. ROWS: IN tests that there is one, SCALAR
/// reads `column` of the only one. COUNT_VALUES counts the non-null values
/// of `column`, COUNT_IF its true ones; the rest aggregate `column`.
enum class RangeRead : std::uint8_t {
    NONE,
    ROWS,
    COUNT,
    COUNT_VALUES,
    COUNT_IF,
    SUM,
    MIN,
    MAX,
    MEAN,
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
    LookupKind kind = LookupKind::IN;
    RangeRead range = RangeRead::NONE;
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
    return l.range == RangeRead::NONE &&
           ((l.kind == LookupKind::IN && !l.key_set) ||
            (l.kind == LookupKind::SCALAR && !l.keys.empty()));
}

struct TMatch {
    TermPtr subject;
    MatchOp op;
    std::string pattern;
    bool negated = false;
    std::optional<char> escape;
    std::shared_ptr<const CompiledPattern> compiled;
};

struct Term {
    std::variant<TConst, TField, TUnary, TBinary, TBetween, TIs, TIn, TCall,
                 TMatch, TQuant, TLookup>
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
            }
        },
        t.node);
}

/// Calls `fn` with every record or outer field under `t`.
template <class F>
void for_each_term_field(const Term& t, F&& fn) {
    for_each_term(t, [&fn](const Term& x) {
        if (const auto* f = std::get_if<TField>(&x.node);
            f && f->root != FieldRoot::ELEMENT)
            fn(*f);
    });
}

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_TERM_H
