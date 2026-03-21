#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_COLUMN_BUILDER_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_COLUMN_BUILDER_H

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/utilities/common/arrow/arrow_export.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::common::arrow {

enum class ColumnType { INT64, UINT64, DOUBLE, STRING, BOOL };

struct ColumnSpec {
    std::string name;
    ColumnType type;
};

struct ColumnData {
    std::string name;
    ColumnType type;
    std::vector<int64_t> int64_values;
    std::vector<uint64_t> uint64_values;
    std::vector<double> double_values;
    std::vector<std::string_view> string_values;
    std::vector<uint8_t> bool_values;
    std::vector<uint8_t> validity;  // 1 = valid, 0 = null
    size_t count = 0;
    bool has_nulls = false;
};

/**
 * Type-safe columnar builder producing Arrow record batches via nanoarrow.
 *
 * Two modes:
 *   Static: declare_schema() once, then append rows. Fastest path.
 *   Dynamic: add_or_get_column() on first encounter; backfills nulls for
 *            columns not touched in a given row via end_row().
 *
 * String columns store std::string_view — caller must keep source data
 * alive until finish() returns.
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
    size_t add_or_get_column(std::string_view name, ColumnType type);

    // Returns the index of an existing column, or std::nullopt if not found.
    // Use before appending null values to avoid creating STRING-typed columns
    // that may later receive typed values.
    std::optional<size_t> find_column(std::string_view name) const;

    // Returns the type of column at col_idx.
    ColumnType column_type(size_t col_idx) const noexcept;

    // Append typed values by column index.
    void append_int64(size_t col_idx, int64_t value);
    void append_uint64(size_t col_idx, uint64_t value);
    void append_double(size_t col_idx, double value);
    void append_string(size_t col_idx, std::string_view value);
    void append_bool(size_t col_idx, bool value);
    void append_null(size_t col_idx);

    // End current row. In dynamic mode, backfills nulls for untouched
    // columns. In static mode, validates all columns were appended.
    // Always increments num_rows_.
    void end_row();

    // Pre-allocate internal buffers for num_rows rows.
    void reserve(size_t num_rows);

    // Bulk-convert internal vectors to Arrow and return a self-contained
    // result. Builder is in an undefined state until reset() is called.
    ArrowExportResult finish();

    // Clear data. If keep_schema is true, column structure is preserved
    // for the next batch (static mode only; dynamic mode always clears).
    void reset(bool keep_schema = true);

    size_t num_rows() const noexcept { return num_rows_; }
    size_t num_columns() const noexcept { return columns_.size(); }

   private:
    std::vector<ColumnData> columns_;
    std::unordered_map<std::string, size_t> name_to_index_;
    size_t num_rows_ = 0;
    bool schema_declared_ = false;
    // Tracks which columns were touched in the current row (dynamic mode).
    std::vector<bool> touched_;

    void init_column(ColumnData& col, ColumnType type, std::string_view name);
    void backfill_nulls(ColumnData& col, size_t target_count);
};

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_COLUMN_BUILDER_H
