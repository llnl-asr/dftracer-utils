#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_COLUMN_BUILDER_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_COLUMN_BUILDER_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>
#include <dftracer/utils/utilities/common/statistics/ddsketch.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::common::arrow {

/// HIST is a list<struct<lo:double, hi:double, count:uint64>> column: one raw
/// histogram (a list of buckets) per row. STRING_LIST is a list<utf8> column
/// and INT64_LIST a list<int64> column: one list per row. STRUCT_LIST is a
/// list<struct<...>> column whose struct fields are named by ColumnSpec::fields
/// (each a scalar column type): one list of inner structs per row.
enum class ColumnType {
    INT64,
    UINT64,
    DOUBLE,
    STRING,
    /// Arrow binary: same offset+data layout as STRING but no UTF-8 validation,
    /// so it carries opaque byte blobs (a plugin BYTES key resolves to one).
    BINARY,
    BOOL,
    DICT_STRING,
    HIST,
    STRING_LIST,
    INT64_LIST,
    STRUCT_LIST,
    INT8,
    INT16,
    INT32,
    UINT8,
    UINT16,
    UINT32,
    FLOAT32
};

struct ColumnSpec {
    std::string name;
    ColumnType type;
    /// Inner struct field specs, used only for STRUCT_LIST (each a scalar
    /// type).
    std::vector<ColumnSpec> fields = {};
};

/// One inner-struct field value for append_struct_list; the member matching the
/// field's ColumnType is read (str for STRING, f64 for DOUBLE/FLOAT32, u64 for
/// unsigned widths, i64 otherwise).
struct StructCell {
    std::int64_t i64 = 0;
    std::uint64_t u64 = 0;
    double f64 = 0.0;
    std::string_view str;
};

struct ColumnData {
    std::string name;
    ColumnType type;
    std::vector<std::int64_t> int64_values;
    std::vector<std::uint64_t> uint64_values;
    std::vector<double> double_values;
    std::vector<std::int32_t> string_offsets;
    std::vector<char> string_data;
    std::vector<std::uint8_t> bool_values;
    std::vector<std::uint8_t> validity;
    std::size_t count = 0;
    bool has_nulls = false;

    /// Dictionary encoding support (for DICT_STRING)
    std::vector<std::int32_t> dict_indices;  ///< indices into dict_values
    std::deque<std::string> dict_values;     ///< unique strings (dictionary)
    std::unordered_map<std::string_view, std::int32_t>
        dict_map;                            ///< string -> index

    /// HIST support: flattened buckets across rows, plus each row's end offset
    /// into hist_bins (parallel to string_offsets).
    std::vector<statistics::HistogramBin> hist_bins;
    std::vector<std::int32_t> hist_offsets;

    /// STRING_LIST support: elements flattened into string_data/string_offsets
    /// (one offset per element), plus each row's end offset into that element
    /// stream. INT64_LIST reuses list_offsets over the flat int64_values
    /// stream.
    std::vector<std::int32_t> list_offsets;

    /// STRUCT_LIST support: struct_fields names/types each inner field;
    /// struct_children[f] holds that field's flat cells across all inner
    /// entries (its primitive vectors only), and list_offsets marks each row's
    /// end offset into that inner-entry stream.
    std::vector<ColumnSpec> struct_fields;
    std::vector<ColumnData> struct_children;
};

/**
 * Type-safe columnar builder producing Arrow record batches via nanoarrow.
 *
 * Two modes:
 *   Static: declare_schema() once, then append rows. Fastest path.
 *   Dynamic: add_or_get_column() on first encounter; backfills nulls for
 *            columns not touched in a given row via end_row().
 *
 * String columns copy and own their data - no lifetime requirements on
 * the source strings passed to append_string().
 *
 * NOT thread-safe. One builder per worker/coroutine.
 */
class RecordBatchBuilder {
   public:
    RecordBatchBuilder() = default;

    /// Static schema mode - call once before any appends.
    void declare_schema(std::initializer_list<ColumnSpec> specs);
    void declare_schema(const std::vector<ColumnSpec>& specs);

    /// Dynamic schema mode - returns column index.
    /// Creates column (backfilling nulls) if it doesn't exist.
    /// Returns existing index if column already exists; type is ignored for
    /// existing columns - callers must use find_column() to check type before
    /// appending, and fall back to append_null() on mismatch.
    std::size_t add_or_get_column(std::string_view name, ColumnType type);

    /// Returns the index of an existing column, or std::nullopt if not found.
    /// Use before appending null values to avoid creating STRING-typed columns
    /// that may later receive typed values.
    std::optional<std::size_t> find_column(std::string_view name) const;

    /// Returns the type of column at col_idx.
    ColumnType column_type(std::size_t col_idx) const noexcept;

    /// Append typed values by column index.
    void append_int64(std::size_t col_idx, std::int64_t value);
    void append_uint64(std::size_t col_idx, std::uint64_t value);
    void append_double(std::size_t col_idx, double value);
    void append_string(std::size_t col_idx, std::string_view value);
    /// Append raw bytes to a BINARY column; shares STRING's storage, so any
    /// byte (including an embedded NUL) is preserved by length, not by NUL.
    void append_binary(std::size_t col_idx, std::string_view value);
    void append_dict_string(std::size_t col_idx, std::string_view value);
    void append_bool(std::size_t col_idx, bool value);
    /// Append one row's histogram (a list of buckets) to a HIST column.
    void append_hist(std::size_t col_idx,
                     const std::vector<statistics::HistogramBin>& bins);
    /// Append one row's list of strings to a STRING_LIST column.
    void append_string_list(std::size_t col_idx,
                            const std::vector<std::string_view>& values);
    /// Append one row's list of int64 values to an INT64_LIST column.
    void append_int64_list(std::size_t col_idx,
                           const std::vector<std::int64_t>& values);
    /// Append one row's list of inner structs to a STRUCT_LIST column; each
    /// inner row carries one StructCell per struct field, in field order.
    void append_struct_list(std::size_t col_idx,
                            const std::vector<std::vector<StructCell>>& rows);
    void append_null(std::size_t col_idx);

    /// End current row. In dynamic mode, backfills nulls for untouched
    /// columns. In static mode, validates all columns were appended.
    /// Always increments num_rows_.
    void end_row();

    /// Pre-allocate internal buffers for num_rows rows.
    void reserve(std::size_t num_rows);

    /// Bulk-convert internal vectors to Arrow and return a self-contained
    /// result. Builder is in an undefined state until reset() is called.
    ArrowExportResult finish();

    /// Clear data. If keep_schema is true, column structure is preserved
    /// for the next batch (requires schema to be locked first).
    void reset(bool keep_schema = true);

    /// Lock the current schema. After locking:
    /// - Existing columns maintain their positions
    /// - New columns discovered via add_or_get_column() are appended at end
    /// - reset(true) preserves the schema structure
    /// Call after emitting the first batch to ensure consistent column
    /// ordering.
    void lock_schema() noexcept { schema_locked_ = true; }

    /// Check if schema is locked.
    bool is_schema_locked() const noexcept { return schema_locked_; }

    std::size_t num_rows() const noexcept { return num_rows_; }
    std::size_t num_columns() const noexcept { return columns_.size(); }

   private:
    std::vector<ColumnData> columns_;
    StringViewMap<std::size_t> name_to_index_;
    std::size_t num_rows_ = 0;
    std::size_t row_touched_count_ = 0;
    bool schema_declared_ = false;
    bool schema_locked_ = false;
    std::vector<bool> touched_;

    void init_column(ColumnData& col, ColumnType type, std::string_view name);
    void backfill_nulls(ColumnData& col, std::size_t target_count);
    // Dynamic-mode first-touch tracking for column col_idx (no-op once the
    // schema is declared or locked).
    void mark_touched(std::size_t col_idx);
};

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_COLUMN_BUILDER_H
