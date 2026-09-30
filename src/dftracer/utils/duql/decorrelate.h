#ifndef DFTRACER_UTILS_DUQL_DECORRELATE_H
#define DFTRACER_UTILS_DUQL_DECORRELATE_H

#include <dftracer/utils/duql/syntax/tree.h>
#include <dftracer/utils/duql/term.h>

#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::duql {

/// Columns a decorrelated side adds: key `i` is `CORRELATED_KEY_PREFIX + i`.
inline constexpr const char* CORRELATED_KEY_PREFIX = "__ck";
/// The column of a scalar side that holds the sub-query's value.
inline constexpr const char* CORRELATED_VALUE = "__correlated_value";
/// The column of a range side that holds the bounded expression.
inline constexpr const char* CORRELATED_RANGE = "__correlated_range";

/// A bound of a correlated range: the enclosing row's expression, with `^.`
/// read as the row itself, and whether the bound itself is excluded.
struct CorrelatedBound {
    syntax::ExprPtr outer;
    bool open = false;
};

/// A sub-query the enclosing row's keys select rows of: it runs once, over
/// every key, and each enclosing row reads the rows of its own keys.
struct Decorrelated {
    /// The sub-query's canonical text, which the spans of `side` and `outer`
    /// index.
    std::string text;
    syntax::PipelinePtr side;
    /// The key of each `where <inner> == ^.<outer>` term, from the enclosing
    /// row, with `^.` read as the row itself.
    std::vector<syntax::ExprPtr> outer;
    /// Columns of `side` that hold the keys, in the order of `outer`.
    std::vector<std::string> names;
    /// For a scalar sub-query ending in `agg`: the sub-query over no rows,
    /// which is what a row with no matching key reads. Null otherwise.
    syntax::PipelinePtr empty;
    /// A range: the bounds of `side`'s CORRELATED_RANGE column, and what the
    /// sub-query gives for its rows in range. `empty` is then null.
    std::optional<CorrelatedBound> low;
    std::optional<CorrelatedBound> high;
    RangeRead read = RangeRead::NONE;
};

/// A construct a correlated sub-query may not hold.
struct Refusal {
    /// The sub-query's canonical text, which `span` indexes.
    std::string text;
    syntax::Span span;
    std::string message;
};

/// The sub-query `p` decorrelated, nullopt when it reads no enclosing row.
/// `scalar` is a sub-query in an expression, else one under `in`. Throws
/// Refusal for a `^.` outside a key or bound term of a `where`, bounds on two
/// expressions, a stage that depends on the other keys' rows after that
/// `where`, a last stage that does not name its columns, or a range sub-query
/// whose last stage has no range form.
std::optional<Decorrelated> decorrelate(const syntax::Pipeline& p, bool scalar);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_DECORRELATE_H
