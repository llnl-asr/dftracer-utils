#ifndef DFTRACER_UTILS_DUQL_LOOKUP_H
#define DFTRACER_UTILS_DUQL_LOOKUP_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/numbers.h>
#include <dftracer/utils/duql/term.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dftracer::utils::dataframe {
struct DataFrame;
}  // namespace dftracer::utils::dataframe

namespace dftracer::utils::duql {

/// Keys follow duql value equality: a number by value (an integral double as
/// the integer, exactly past 2^53), a string by its bytes, a bool apart; a
/// tuple concatenates its parts. Null and missing values have no key.
void append_int_key(std::string& out, bool negative, std::uint64_t magnitude);
/// False for NaN, which has no key.
bool append_double_key(std::string& out, double v);
void append_string_key(std::string& out, std::string_view v);
void append_bool_key(std::string& out, bool v);
/// `key` as duql text, for messages.
std::string key_text(std::string_view key);

/// A scalar as duql orders it: numbers by value across their types, strings
/// by bytes, false before true. Values of different kinds do not compare.
using Ordered =
    std::variant<std::int64_t, std::uint64_t, double, std::string_view, bool>;

/// -1/0/1 of `a` and `b` in the order of kinds number, string, bool, then
/// by value within a kind. NaN is not an Ordered.
int compare_ordered(const Ordered& a, const Ordered& b);

__extension__ typedef __int128 RangeWide;

/// A range lookup's rows: each key's rows sorted by range value, and over
/// them a segment tree of the RangeRead's aggregate.
struct RangeIndex {
    struct Node {
        RangeWide isum = 0;
        double fsum = 0;
        /// Non-null values; COUNT_IF: true ones.
        std::int64_t count = 0;
        /// MIN, MAX: the position of the best value, -1 for none.
        std::int64_t best = -1;
    };
    /// By position: the frame row and its range value; MIN, MAX: its value.
    std::vector<std::int64_t> rows;
    std::vector<Ordered> values;
    std::vector<std::optional<Ordered>> args;
    /// Each key's positions [first, second).
    StringViewMap<std::pair<std::int64_t, std::int64_t>> keys;
    /// The tree of the key at [b, e) is at [2b, 2e), leaf i at 2b + (e - b)
    /// + i; empty for ROWS and COUNT.
    std::vector<Node> tree;
    /// SUM, MEAN: the value column holds integers.
    bool integral = false;
    /// The strings of `values` and `args` read from JSON cells.
    std::deque<std::string> owned;
};

/// Lays out `leaves` (one per position of `index`) as each key's tree.
void build_range_tree(RangeIndex& index, RangeRead read,
                      const std::vector<RangeIndex::Node>& leaves);

/// A collected row set with its rows by key. Its columns are FLAT.
struct LookupTable {
    std::string name;
    std::shared_ptr<const dataframe::DataFrame> frame;
    StringViewMap<std::vector<std::int64_t>> rows;
    /// The columns the rows are keyed by.
    std::vector<std::size_t> key_columns;
    /// ARROW: the column the lookup reads, none when the row set lacks it.
    std::optional<std::size_t> value;
    /// The cells of `value` (SCALAR: of the only column) for the row
    /// evaluator; empty for a list or object cell, which it cannot read.
    std::vector<std::optional<Cell>> cells;
    /// A range lookup's rows by key and range value; `rows` is then empty.
    std::shared_ptr<const RangeIndex> range;
    const std::vector<std::int64_t>* find(std::string_view key) const;
};

/// What a range lookup gives for one enclosing row: the number of rows in
/// range, and its RangeRead's value: a number (COUNT*, SUM, MEAN) or the
/// frame row that holds it (ROWS: the first row in range, MIN, MAX); neither
/// when it is null.
struct RangeAnswer {
    std::int64_t rows = 0;
    std::optional<Number> number;
    std::int64_t row = -1;
};

/// The rows of `table` with `key` between the bounds `low` and `high` of the
/// range lookup `l`, a bound being nullopt when its value has no order (null,
/// NaN, a container). Throws the query error naming the sub-query when an
/// integer SUM passes int64.
RangeAnswer range_answer(const TLookup& l, const LookupTable& table,
                         std::string_view key,
                         const std::optional<Ordered>& low,
                         const std::optional<Ordered>& high);

/// Throws the query error for rows of `table` whose `column` differ at `key`.
[[noreturn]] void conflict(const LookupTable& table, std::string_view column,
                           std::string_view key);

/// Throws the query error for the sub-query `name` that gives `rows` rows at
/// `key`.
[[noreturn]] void scalar_conflict(std::string_view name, std::string_view key,
                                  std::size_t rows);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_LOOKUP_H
