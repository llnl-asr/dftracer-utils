// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_ARROW_COLUMN_BUILDER_H
#define DFTRACER_UTILS_UTILITIES_ARROW_COLUMN_BUILDER_H

#include <dftracer/utils/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/arrow/arrow_export.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace dftracer::utils::utilities::arrow {

/// HIST is a list<struct<lo:double, hi:double, count:uint64>> column: one raw
/// histogram (a list of buckets) per row. STRING_LIST is a list<utf8> column
/// and INT64_LIST a list<int64> column: one list per row. STRUCT_LIST is a
/// list<struct<...>> column whose struct fields are named by ColumnSpec::fields
/// (each a scalar column type): one list of inner structs per row.
enum class ColumnType {
    Int64,
    Uint64,
    Double,
    String,
    /// Arrow binary: same offset+data layout as STRING but no UTF-8 validation,
    /// so it carries opaque byte blobs (a plugin BYTES key resolves to one).
    Binary,
    Bool,
    DictString,
    Hist,
    StringList,
    Int64List,
    StructList,
    Int8,
    Int16,
    Int32,
    Uint8,
    Uint16,
    Uint32,
    Float32
};

/// Which backing vectors of @ref ColumnData a @ref ColumnType uses. The
/// fixed-width integer and float variants share their 64-bit accumulator.
enum class StorageKind : std::uint8_t {
    Int64,
    Uint64,
    Double,
    String,
    Bool,
    Dict,
    Hist,
    StringList,
    Int64List,
    StructList
};

struct ColumnSpec {
    dstd::String name;
    ColumnType type;
    /// Inner struct field specs, used only for STRUCT_LIST (each a scalar
    /// type).
    dstd::Vector<ColumnSpec> fields = {};
};

/// One inner-struct field value for append_struct_list; the member matching the
/// field's ColumnType is read (str for STRING, f64 for DOUBLE/FLOAT32, u64 for
/// unsigned widths, i64 otherwise).
struct StructCell {
    std::int64_t i64 = 0;
    std::uint64_t u64 = 0;
    double f64 = 0.0;
    dstd::StringView str;
};

struct ColumnData {
    dstd::String name;
    ColumnType type;
    /// The storage of `type`; each append checks it before it writes.
    StorageKind kind = StorageKind::Int64;
    dstd::Vector<std::int64_t> int64_values;
    dstd::Vector<std::uint64_t> uint64_values;
    dstd::Vector<double> double_values;
    dstd::Vector<std::int32_t> string_offsets;
    dstd::Vector<char> string_data;
    dstd::Vector<std::uint8_t> bool_values;
    dstd::Vector<std::uint8_t> validity;
    std::size_t count = 0;
    bool has_nulls = false;

    /// Dictionary encoding support (for DICT_STRING)
    dstd::Vector<std::int32_t> dict_indices;  ///< indices into dict_values
    dstd::Deque<dstd::String> dict_values;    ///< unique strings (dictionary)
    dstd::HashMap<dstd::StringView, std::int32_t>
        dict_map;                             ///< string -> index

    /// HIST support: flattened buckets across rows, plus each row's end offset
    /// into hist_bins (parallel to string_offsets).
    dstd::Vector<dataframe::HistogramBin> hist_bins;
    dstd::Vector<std::int32_t> hist_offsets;

    /// STRING_LIST support: elements flattened into string_data/string_offsets
    /// (one offset per element), plus each row's end offset into that element
    /// stream. INT64_LIST reuses list_offsets over the flat int64_values
    /// stream.
    dstd::Vector<std::int32_t> list_offsets;

    /// STRUCT_LIST support: struct_fields names/types each inner field;
    /// struct_children[f] holds that field's flat cells across all inner
    /// entries (its primitive vectors only), and list_offsets marks each row's
    /// end offset into that inner-entry stream.
    dstd::Vector<ColumnSpec> struct_fields;
    dstd::Vector<ColumnData> struct_children;
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
    void declare_schema(const dstd::Vector<ColumnSpec>& specs);

    /// Dynamic schema mode - returns column index.
    /// Creates column (backfilling nulls) if it doesn't exist.
    ///
    /// Returns the existing index if the column exists. When `type` differs
    /// from the column's type and the schema is not declared, the column is
    /// promoted so that no value is lost: mixed Int64, Uint64 and Double make
    /// it Double, and any mix with a String, Bool or DictString makes it
    /// String, where a number or bool is its JSON text. Rows already appended
    /// are converted. A locked schema promotes too, and a later batch keeps the
    /// wider type. The returned index stays valid after a later promotion, and
    /// every append_* accepts a value of any scalar type: it converts the value
    /// to the column's current type.
    ///
    /// Promotion is lossy in two ways. An integer above 2^53 in magnitude
    /// becomes the nearest double, which is inexact. A number that becomes text
    /// is written in a normalized form, not its source text: `1.50` is `1.5`,
    /// a whole double keeps a `.0` (`2.0`), and a non-finite double is
    /// `Infinity`, `-Infinity` or `NaN`, which is no JSON.
    ///
    /// A declared schema keeps its types. A value the column can hold without
    /// loss is converted (an integer into Double, a number, bool or string
    /// into String); any other value is stored as a null.
    std::size_t add_or_get_column(dstd::StringView name, ColumnType type);

    /// Returns the index of an existing column, or dstd::nullopt if not found.
    /// Use before appending null values to avoid creating STRING-typed columns
    /// that may later receive typed values.
    dstd::Optional<std::size_t> find_column(dstd::StringView name) const;

    /// Returns the type of column at col_idx.
    ColumnType column_type(std::size_t col_idx) const noexcept;

    /// Append typed values by column index.
    void append_int64(std::size_t col_idx, std::int64_t value);
    void append_uint64(std::size_t col_idx, std::uint64_t value);
    void append_double(std::size_t col_idx, double value);
    void append_string(std::size_t col_idx, dstd::StringView value);
    /// Append raw bytes to a BINARY column; shares STRING's storage, so any
    /// byte (including an embedded NUL) is preserved by length, not by NUL.
    void append_binary(std::size_t col_idx, dstd::StringView value);
    void append_dict_string(std::size_t col_idx, dstd::StringView value);
    void append_bool(std::size_t col_idx, bool value);
    /// Append one row's histogram (a list of buckets) to a HIST column.
    void append_hist(std::size_t col_idx,
                     const dstd::Vector<dataframe::HistogramBin>& bins);
    /// Append one row's list of strings to a STRING_LIST column.
    void append_string_list(std::size_t col_idx,
                            const dstd::Vector<dstd::StringView>& values);
    /// Append one row's list of int64 values to an INT64_LIST column.
    void append_int64_list(std::size_t col_idx,
                           const dstd::Vector<std::int64_t>& values);
    /// Append one row's list of inner structs to a STRUCT_LIST column; each
    /// inner row carries one @ref StructCell per struct field, in field order.
    void append_struct_list(std::size_t col_idx,
                            const dstd::Vector<dstd::Vector<StructCell>>& rows);
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
    dstd::Vector<ColumnData> columns_;
    dstd::StringViewMap<std::size_t> name_to_index_;
    std::size_t num_rows_ = 0;
    std::size_t row_touched_count_ = 0;
    bool schema_declared_ = false;
    bool schema_locked_ = false;
    dstd::Vector<bool> touched_;

    struct Scalar;

    void init_column(ColumnData& col, ColumnType type, dstd::StringView name);
    void promote_column(ColumnData& col, ColumnType target);
    // The slow path of the append_* calls: `col_idx` holds another type than
    // the value, so the value is converted or stored as a null.
    void append_scalar_slow(std::size_t col_idx, const Scalar& value);
    void backfill_nulls(ColumnData& col, std::size_t target_count);
    // Dynamic-mode first-touch tracking for column col_idx (no-op once the
    // schema is declared or locked).
    void mark_touched(std::size_t col_idx);
};

}  // namespace dftracer::utils::utilities::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_UTILITIES_ARROW_COLUMN_BUILDER_H
