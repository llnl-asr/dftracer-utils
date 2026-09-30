#ifndef DFTRACER_UTILS_DUQL_VECTORIZE_H
#define DFTRACER_UTILS_DUQL_VECTORIZE_H

#include <dftracer/utils/core/common/expected.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/duql/lookup.h>
#include <dftracer/utils/duql/term.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::duql {

/// A column of the frame a term is compiled against; `col(i)` is the i-th.
/// `json` marks a declared json field, which the frame holds as JSON text.
/// `record` marks a column read from records, where a null cell may be a
/// missing field, so `is missing` and `exists()` test the cell for null.
/// `quant` marks a column that holds the truth of that quantifier (see
/// quantify()); a quantifier compiles only to such a column. `lookup` marks
/// the column that holds that lookup's values (see lookup()). In a
/// quantifier's element frame, the element is the column named `.` and a
/// field of a struct element the column `.<field>`. `call` marks the column
/// that holds that call's values (see call_column()).
struct VectorColumn {
    std::string name;
    dataframe::DataType type;
    bool json = false;
    bool record = false;
    const TQuant* quant = nullptr;
    const TLookup* lookup = nullptr;
    const TCall* call = nullptr;
};

struct VectorizeError {
    std::string message;
    /// The term reads a json column, which only the row evaluator can walk.
    bool needs_rows = false;
};

/// `t` as a column expression over `columns` with duql's meaning: an unknown
/// result is a null cell. A field that no column names, and an operand of the
/// wrong type, compile to a null column. With `args_fallback`, a bare name no
/// column names reads the column of that name under `args.`; the same holds
/// for the functions below.
dftracer::utils::expected<dataframe::Expr, VectorizeError> vectorize(
    const Term& t, const std::vector<VectorColumn>& columns,
    bool args_fallback);

/// `t` as a Bool column: its truth, null where it is not a bool.
dftracer::utils::expected<dataframe::Expr, VectorizeError> vectorize_condition(
    const Term& t, const std::vector<VectorColumn>& columns,
    bool args_fallback);

/// The truth of `q` for each row of `inputs`, the frame `columns` describes:
/// its condition runs over a frame of the list elements with the record's
/// columns repeated per element, then each row reduces with the any or all
/// rule. Null where the subject is not a list or the result is unknown. A
/// subject that is a lookup of every match reads the matching rows' values,
/// null where none matches.
dftracer::utils::expected<dataframe::Series, VectorizeError> quantify(
    const TQuant& q, const std::vector<const dataframe::Series*>& inputs,
    const std::vector<VectorColumn>& columns, bool args_fallback);

/// The column the bound lookup `l` (not `l.all`) gives for each row of
/// `inputs`. Throws DFTUtilsException for a query error of the lookup.
dftracer::utils::expected<dataframe::Series, VectorizeError> lookup(
    const TLookup& l, const std::vector<const dataframe::Series*>& inputs,
    const std::vector<VectorColumn>& columns, bool args_fallback);

/// Whether `fn` runs on columns only through call_column(): slice, flatten,
/// keys, values, parse_json and split.
bool is_column_call(Fn fn);

/// The type call_column() gives `c` over arguments of `args` types; for keys
/// and values, the types of the object's fields. Nullopt when every row is
/// unknown. Throws std::invalid_argument for values() of fields with no
/// common type.
std::optional<dataframe::DataType> call_type(
    const TCall& c, const std::vector<dataframe::DataType>& args);

/// The column of `c` over its arguments' columns `args`, as `type`. For keys
/// and values, `args` are the object's fields named `keys`, or its one struct
/// column. parse_json gives canonical JSON text.
dataframe::Series call_column(const TCall& c,
                              const std::vector<dataframe::Series>& args,
                              const std::vector<std::string>& keys,
                              const dataframe::DataType& type);

/// The key of row `r` of the FLAT column `s`; false when it has none.
bool append_cell_key(std::string& out, const dataframe::Series& s,
                     std::int64_t r);

/// A frame with every column FLAT, for LookupTable.
dataframe::DataFrame flat_frame(dataframe::DataFrame f);

/// The bytes the buffers of the FLAT frame `f` hold.
std::uint64_t frame_bytes(const dataframe::DataFrame& f);

/// The column of `f` named `name`, or `args.<name>`.
std::optional<std::size_t> column_of(const dataframe::DataFrame& f,
                                     std::string_view name);

/// The table `t` reads over `frame`, keyed by `t.target` (a range IN: every
/// column but the range). Throws DFTUtilsException naming the row set when a
/// key column is missing, an uncorrelated SCALAR side is not one row and one
/// column, or `t` is joined().
std::shared_ptr<const LookupTable> make_lookup_table(
    const TLookup& t, std::shared_ptr<const dataframe::DataFrame> frame);

/// A table over `frame` keyed by `columns`.
std::shared_ptr<const LookupTable> make_lookup_table(
    std::string name, std::shared_ptr<const dataframe::DataFrame> frame,
    const std::vector<std::size_t>& columns);

/// Each row's matching rows: row r's are rows[offsets[r], offsets[r + 1]);
/// keyed[r] is 0 when a key part is null or missing.
struct Matches {
    std::vector<std::int64_t> offsets;
    std::vector<std::int64_t> rows;
    std::vector<std::uint8_t> keyed;
};

/// The key of row `r` of the key columns `keys`, for messages.
std::string row_key(const std::vector<const dataframe::Series*>& keys,
                    std::int64_t r);

/// The matches in `table` of each of the `n` rows of the key columns `keys`.
Matches match(const LookupTable& table,
              const std::vector<const dataframe::Series*>& keys,
              std::int64_t n);

/// The column the bound lookup `t` gives for `n` rows with key columns
/// `keys`: IN a Bool, ARROW the value (throwing, naming the row set and the
/// key, when matching rows differ), SCALAR its cell. Not for `t.all`.
dataframe::Series lookup_column(
    const TLookup& t, const std::vector<const dataframe::Series*>& keys,
    std::int64_t n);

/// A joined `in` over `matched`, the marker its lookup join attached (null
/// where no side row matched): TRUE on a match (FALSE when `negated`), the
/// opposite without one, null when one of the first `subject` key columns
/// has no value.
dataframe::Series in_column(const std::vector<const dataframe::Series*>& keys,
                            std::size_t subject,
                            const dataframe::Series& matched, bool negated);

/// A joined keyed scalar over `nest`, the side rows its nest join gathered:
/// the value of struct field `field` of the one row, the CORRELATED_VALUE of
/// the one-row `empty` (else null) for none. Throws the query error naming
/// the sub-query `name` and the key for several rows.
dataframe::Series scalar_column(
    const std::string& name, const std::vector<const dataframe::Series*>& keys,
    const dataframe::Series& nest, std::size_t field,
    const dataframe::DataFrame* empty);

/// Row `a` of `x` equals row `b` of `y` as duql values; null equals null.
bool same_cell(const dataframe::Series& x, std::int64_t a,
               const dataframe::Series& y, std::int64_t b);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_VECTORIZE_H
