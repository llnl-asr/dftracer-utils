#ifndef DFTRACER_UTILS_DUQL_LOOKUP_H
#define DFTRACER_UTILS_DUQL_LOOKUP_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/term.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
    const std::vector<std::int64_t>* find(std::string_view key) const;
};

/// Throws the query error for rows of `table` whose `column` differ at `key`.
[[noreturn]] void conflict(const LookupTable& table, std::string_view column,
                           std::string_view key);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_LOOKUP_H
