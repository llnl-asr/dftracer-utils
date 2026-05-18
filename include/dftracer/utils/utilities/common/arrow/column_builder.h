#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_COLUMN_BUILDER_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_COLUMN_BUILDER_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>

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

enum class ColumnType { INT64, UINT64, DOUBLE, STRING, BOOL, DICT_STRING };

struct ColumnSpec {
    std::string name;
    ColumnType type;
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

    // Dictionary encoding support (for DICT_STRING)
    std::vector<std::int32_t> dict_indices;  // indices into dict_values
    std::deque<std::string> dict_values;     // unique strings (dictionary)
    std::unordered_map<std::string_view, std::int32_t>
        dict_map;                            // string -> index
};

/**
 * Type-safe columnar builder producing Arrow record batches via nanoarrow.
 *
 * Two modes:
 *   Static: declare_schema() once, then append rows. Fastest path.
 *   Dynamic: add_or_get_column() on first encounter; backfills nulls for
 *            columns not touched in a given row via end_row().
 *
 * String columns copy and own their data — no lifetime requirements on
 * the source strings passed to append_string().
 *
 * NOT thread-safe. One builder per worker/coroutine.
 */
class RecordBatchBuilder {
   public:
    RecordBatchBuilder() = default;

    // Static schema mode — call once before any appends.
    void declare_schema(std::initializer_list<ColumnSpec> specs);
    void declare_schema(const std::vector<ColumnSpec>& specs);

    // Dynamic schema mode — returns column index.
    // Creates column (backfilling nulls) if it doesn't exist.
    // Returns existing index if column already exists; type is ignored for
    // existing columns — callers must use find_column() to check type before
    // appending, and fall back to append_null() on mismatch.
    std::size_t add_or_get_column(std::string_view name, ColumnType type);

    // Returns the index of an existing column, or std::nullopt if not found.
    // Use before appending null values to avoid creating STRING-typed columns
    // that may later receive typed values.
    std::optional<std::size_t> find_column(std::string_view name) const;

    // Returns the type of column at col_idx.
    ColumnType column_type(std::size_t col_idx) const noexcept;

    // Append typed values by column index.
    void append_int64(std::size_t col_idx, std::int64_t value);
    void append_uint64(std::size_t col_idx, std::uint64_t value);
    void append_double(std::size_t col_idx, double value);
    void append_string(std::size_t col_idx, std::string_view value);
    void append_dict_string(std::size_t col_idx, std::string_view value);
    void append_bool(std::size_t col_idx, bool value);
    void append_null(std::size_t col_idx);

    // End current row. In dynamic mode, backfills nulls for untouched
    // columns. In static mode, validates all columns were appended.
    // Always increments num_rows_.
    void end_row();

    // Pre-allocate internal buffers for num_rows rows.
    void reserve(std::size_t num_rows);

    // Bulk-convert internal vectors to Arrow and return a self-contained
    // result. Builder is in an undefined state until reset() is called.
    ArrowExportResult finish();

    // Clear data. If keep_schema is true, column structure is preserved
    // for the next batch (requires schema to be locked first).
    void reset(bool keep_schema = true);

    // Lock the current schema. After locking:
    // - Existing columns maintain their positions
    // - New columns discovered via add_or_get_column() are appended at end
    // - reset(true) preserves the schema structure
    // Call after emitting the first batch to ensure consistent column ordering.
    void lock_schema() noexcept { schema_locked_ = true; }

    // Check if schema is locked.
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
};

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_COLUMN_BUILDER_H
