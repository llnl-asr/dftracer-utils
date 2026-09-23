#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/error.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <nanoarrow/nanoarrow.h>

#include <optional>
#include <stdexcept>
#include <string>

namespace dftracer::utils::utilities::common::arrow {

namespace {

ArrowType to_nanoarrow_type(ColumnType t) noexcept {
    switch (t) {
        case ColumnType::INT64:
            return NANOARROW_TYPE_INT64;
        case ColumnType::UINT64:
            return NANOARROW_TYPE_UINT64;
        case ColumnType::DOUBLE:
            return NANOARROW_TYPE_DOUBLE;
        case ColumnType::STRING:
            return NANOARROW_TYPE_STRING;
        case ColumnType::BINARY:
            return NANOARROW_TYPE_BINARY;
        case ColumnType::BOOL:
            return NANOARROW_TYPE_BOOL;
        case ColumnType::DICT_STRING:
            // Dictionary uses INT32 indices; dictionary values handled
            // separately
            return NANOARROW_TYPE_INT32;
        case ColumnType::HIST:
            return NANOARROW_TYPE_LIST;
        case ColumnType::STRING_LIST:
            return NANOARROW_TYPE_LIST;
        case ColumnType::INT64_LIST:
            return NANOARROW_TYPE_LIST;
        case ColumnType::STRUCT_LIST:
            return NANOARROW_TYPE_LIST;
        case ColumnType::INT8:
            return NANOARROW_TYPE_INT8;
        case ColumnType::INT16:
            return NANOARROW_TYPE_INT16;
        case ColumnType::INT32:
            return NANOARROW_TYPE_INT32;
        case ColumnType::UINT8:
            return NANOARROW_TYPE_UINT8;
        case ColumnType::UINT16:
            return NANOARROW_TYPE_UINT16;
        case ColumnType::UINT32:
            return NANOARROW_TYPE_UINT32;
        case ColumnType::FLOAT32:
            return NANOARROW_TYPE_FLOAT;
    }
    return NANOARROW_TYPE_UNINITIALIZED;
}

// Which backing storage a ColumnType uses. Collapses the fixed-width integer
// and float variants onto their 64-bit accumulator, so the resize / push /
// reserve switches classify once here instead of re-listing every width.
enum class StorageKind {
    INT64,
    UINT64,
    DOUBLE,
    STRING,
    BOOL,
    DICT,
    HIST,
    STRING_LIST,
    INT64_LIST,
    STRUCT_LIST
};

constexpr StorageKind storage_kind(ColumnType t) noexcept {
    switch (t) {
        case ColumnType::INT64:
        case ColumnType::INT8:
        case ColumnType::INT16:
        case ColumnType::INT32:
            return StorageKind::INT64;
        case ColumnType::UINT64:
        case ColumnType::UINT8:
        case ColumnType::UINT16:
        case ColumnType::UINT32:
            return StorageKind::UINT64;
        case ColumnType::DOUBLE:
        case ColumnType::FLOAT32:
            return StorageKind::DOUBLE;
        case ColumnType::STRING:
        case ColumnType::BINARY:
            return StorageKind::STRING;
        case ColumnType::BOOL:
            return StorageKind::BOOL;
        case ColumnType::DICT_STRING:
            return StorageKind::DICT;
        case ColumnType::HIST:
            return StorageKind::HIST;
        case ColumnType::STRING_LIST:
            return StorageKind::STRING_LIST;
        case ColumnType::INT64_LIST:
            return StorageKind::INT64_LIST;
        case ColumnType::STRUCT_LIST:
            return StorageKind::STRUCT_LIST;
    }
    return StorageKind::INT64;
}

}  // namespace

void RecordBatchBuilder::init_column(ColumnData& col, ColumnType type,
                                     std::string_view name) {
    col.name = std::string(name);
    col.type = type;
    col.count = 0;
    col.has_nulls = false;
}

void RecordBatchBuilder::backfill_nulls(ColumnData& col,
                                        std::size_t target_count) {
    std::size_t n = target_count - col.count;
    if (n == 0) return;

    if (!col.has_nulls) {
        col.has_nulls = true;
        col.validity.assign(col.count, 1);
    }
    col.validity.resize(col.count + n, 0);

    switch (storage_kind(col.type)) {
        case StorageKind::INT64:
            col.int64_values.resize(col.count + n, 0);
            break;
        case StorageKind::UINT64:
            col.uint64_values.resize(col.count + n, 0);
            break;
        case StorageKind::DOUBLE:
            col.double_values.resize(col.count + n, 0.0);
            break;
        case StorageKind::STRING:
            col.string_offsets.resize(
                col.count + n,
                static_cast<std::int32_t>(col.string_data.size()));
            break;
        case StorageKind::BOOL:
            col.bool_values.resize(col.count + n, 0);
            break;
        case StorageKind::DICT:
            col.dict_indices.resize(col.count + n, -1);  // -1 = null
            break;
        case StorageKind::HIST:
            // Null rows contribute no buckets: repeat the current end offset.
            col.hist_offsets.resize(
                col.count + n, static_cast<std::int32_t>(col.hist_bins.size()));
            break;
        case StorageKind::STRING_LIST:
            col.list_offsets.resize(
                col.count + n,
                static_cast<std::int32_t>(col.string_offsets.size()));
            break;
        case StorageKind::INT64_LIST:
            col.list_offsets.resize(
                col.count + n,
                static_cast<std::int32_t>(col.int64_values.size()));
            break;
        case StorageKind::STRUCT_LIST: {
            const std::int32_t end = static_cast<std::int32_t>(
                col.struct_children.empty() ? 0 : col.struct_children[0].count);
            col.list_offsets.resize(col.count + n, end);
            break;
        }
    }
    col.count += n;
}

void RecordBatchBuilder::declare_schema(
    std::initializer_list<ColumnSpec> specs) {
    declare_schema(std::vector<ColumnSpec>(specs));
}

void RecordBatchBuilder::declare_schema(const std::vector<ColumnSpec>& specs) {
    columns_.clear();
    name_to_index_.clear();
    columns_.reserve(specs.size());
    touched_.assign(specs.size(), false);

    for (const auto& spec : specs) {
        std::size_t idx = columns_.size();
        columns_.emplace_back();
        init_column(columns_.back(), spec.type, spec.name);
        if (spec.type == ColumnType::STRUCT_LIST) {
            ColumnData& col = columns_.back();
            col.struct_fields = spec.fields;
            col.struct_children.resize(spec.fields.size());
            for (std::size_t f = 0; f < spec.fields.size(); ++f)
                init_column(col.struct_children[f], spec.fields[f].type,
                            spec.fields[f].name);
        }
        name_to_index_[spec.name] = idx;
    }
    schema_declared_ = true;
}

std::size_t RecordBatchBuilder::add_or_get_column(std::string_view name,
                                                  ColumnType type) {
    auto it = name_to_index_.find(name);
    if (it != name_to_index_.end()) {
        // Existing column: type is ignored. Callers that need type-safe
        // appends should use find_column() + column_type() first.
        return it->second;
    }

    std::size_t idx = columns_.size();
    columns_.emplace_back();
    init_column(columns_.back(), type, name);
    if (num_rows_ > 0) {
        backfill_nulls(columns_.back(), num_rows_);
    }
    name_to_index_.emplace(std::string(name), idx);
    touched_.push_back(false);
    return idx;
}

std::optional<std::size_t> RecordBatchBuilder::find_column(
    std::string_view name) const {
    auto it = name_to_index_.find(name);
    if (it != name_to_index_.end()) return it->second;
    return std::nullopt;
}

ColumnType RecordBatchBuilder::column_type(std::size_t col_idx) const noexcept {
    return columns_[col_idx].type;
}

void RecordBatchBuilder::mark_touched(std::size_t col_idx) {
    if (!schema_declared_ && !schema_locked_ && !touched_[col_idx]) {
        touched_[col_idx] = true;
        ++row_touched_count_;
    }
}

void RecordBatchBuilder::append_int64(std::size_t col_idx, std::int64_t value) {
    auto& col = columns_[col_idx];
    col.int64_values.push_back(value);
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_uint64(std::size_t col_idx,
                                       std::uint64_t value) {
    auto& col = columns_[col_idx];
    col.uint64_values.push_back(value);
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_double(std::size_t col_idx, double value) {
    auto& col = columns_[col_idx];
    col.double_values.push_back(value);
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_string(std::size_t col_idx,
                                       std::string_view value) {
    auto& col = columns_[col_idx];
    col.string_data.insert(col.string_data.end(), value.begin(), value.end());
    col.string_offsets.push_back(
        static_cast<std::int32_t>(col.string_data.size()));
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_binary(std::size_t col_idx,
                                       std::string_view value) {
    // BINARY shares STRING's storage and append_string does no UTF-8
    // validation, so the two paths are identical.
    append_string(col_idx, value);
}

void RecordBatchBuilder::append_dict_string(std::size_t col_idx,
                                            std::string_view value) {
    auto& col = columns_[col_idx];
    auto it = col.dict_map.find(value);
    std::int32_t idx;
    if (it != col.dict_map.end()) {
        idx = it->second;
    } else {
        idx = static_cast<std::int32_t>(col.dict_values.size());
        col.dict_values.emplace_back(value);
        // Map key must point to stable storage (dict_values)
        col.dict_map[col.dict_values.back()] = idx;
    }
    col.dict_indices.push_back(idx);
    col.validity.push_back(1);
    ++col.count;
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_bool(std::size_t col_idx, bool value) {
    auto& col = columns_[col_idx];
    col.bool_values.push_back(value ? 1 : 0);
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_hist(
    std::size_t col_idx, const std::vector<statistics::HistogramBin>& bins) {
    auto& col = columns_[col_idx];
    col.hist_bins.insert(col.hist_bins.end(), bins.begin(), bins.end());
    col.hist_offsets.push_back(static_cast<std::int32_t>(col.hist_bins.size()));
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_string_list(
    std::size_t col_idx, const std::vector<std::string_view>& values) {
    auto& col = columns_[col_idx];
    for (std::string_view v : values) {
        col.string_data.insert(col.string_data.end(), v.begin(), v.end());
        col.string_offsets.push_back(
            static_cast<std::int32_t>(col.string_data.size()));
    }
    col.list_offsets.push_back(
        static_cast<std::int32_t>(col.string_offsets.size()));
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_int64_list(
    std::size_t col_idx, const std::vector<std::int64_t>& values) {
    auto& col = columns_[col_idx];
    col.int64_values.insert(col.int64_values.end(), values.begin(),
                            values.end());
    col.list_offsets.push_back(
        static_cast<std::int32_t>(col.int64_values.size()));
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_struct_list(
    std::size_t col_idx, const std::vector<std::vector<StructCell>>& rows) {
    auto& col = columns_[col_idx];
    for (const auto& inner : rows) {
        for (std::size_t f = 0; f < col.struct_children.size(); ++f) {
            ColumnData& fc = col.struct_children[f];
            const StructCell& cell = inner[f];
            switch (storage_kind(fc.type)) {
                case StorageKind::DOUBLE:
                    fc.double_values.push_back(cell.f64);
                    break;
                case StorageKind::UINT64:
                    fc.uint64_values.push_back(cell.u64);
                    break;
                case StorageKind::STRING:
                    fc.string_data.insert(fc.string_data.end(),
                                          cell.str.begin(), cell.str.end());
                    fc.string_offsets.push_back(
                        static_cast<std::int32_t>(fc.string_data.size()));
                    break;
                default:
                    fc.int64_values.push_back(cell.i64);
                    break;
            }
            ++fc.count;
        }
    }
    col.list_offsets.push_back(static_cast<std::int32_t>(
        col.struct_children.empty() ? 0 : col.struct_children[0].count));
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_null(std::size_t col_idx) {
    auto& col = columns_[col_idx];
    if (!col.has_nulls) {
        col.has_nulls = true;
        col.validity.assign(col.count, 1);
    }
    col.validity.push_back(0);

    switch (storage_kind(col.type)) {
        case StorageKind::INT64:
            col.int64_values.push_back(0);
            break;
        case StorageKind::UINT64:
            col.uint64_values.push_back(0);
            break;
        case StorageKind::DOUBLE:
            col.double_values.push_back(0.0);
            break;
        case StorageKind::STRING:
            col.string_offsets.push_back(
                static_cast<std::int32_t>(col.string_data.size()));
            break;
        case StorageKind::BOOL:
            col.bool_values.push_back(0);
            break;
        case StorageKind::DICT:
            col.dict_indices.push_back(-1);  // -1 = null
            break;
        case StorageKind::HIST:
            col.hist_offsets.push_back(
                static_cast<std::int32_t>(col.hist_bins.size()));
            break;
        case StorageKind::STRING_LIST:
            col.list_offsets.push_back(
                static_cast<std::int32_t>(col.string_offsets.size()));
            break;
        case StorageKind::INT64_LIST:
            col.list_offsets.push_back(
                static_cast<std::int32_t>(col.int64_values.size()));
            break;
        case StorageKind::STRUCT_LIST:
            col.list_offsets.push_back(static_cast<std::int32_t>(
                col.struct_children.empty() ? 0
                                            : col.struct_children[0].count));
            break;
    }
    ++col.count;
    mark_touched(col_idx);
}

void RecordBatchBuilder::end_row() {
    if (!schema_declared_ && !schema_locked_ &&
        row_touched_count_ == columns_.size()) {
        std::fill(touched_.begin(), touched_.end(), false);
        row_touched_count_ = 0;
        ++num_rows_;
        return;
    }
    const std::size_t target = num_rows_ + 1;
    const bool reset_touched = !schema_declared_ && !schema_locked_;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].count < target) {
            backfill_nulls(columns_[i], target);
        }
        if (reset_touched) touched_[i] = false;
    }
    row_touched_count_ = 0;
    ++num_rows_;
}

void RecordBatchBuilder::reserve(std::size_t num_rows) {
    for (auto& col : columns_) {
        switch (storage_kind(col.type)) {
            case StorageKind::INT64:
                col.int64_values.reserve(num_rows);
                break;
            case StorageKind::UINT64:
                col.uint64_values.reserve(num_rows);
                break;
            case StorageKind::DOUBLE:
                col.double_values.reserve(num_rows);
                break;
            case StorageKind::STRING:
                col.string_offsets.reserve(num_rows + 1);
                // dftracer hash strings are 16 bytes; common strings
                // (event names, categories) range 4-32. Bumping the
                // initial reservation cuts geometric-growth memmove churn
                // visible in perf for moderate batch sizes.
                col.string_data.reserve(num_rows * 32);
                break;
            case StorageKind::BOOL:
                col.bool_values.reserve(num_rows);
                break;
            case StorageKind::DICT:
                col.dict_indices.reserve(num_rows);
                break;
            case StorageKind::HIST:
                col.hist_offsets.reserve(num_rows);
                break;
            case StorageKind::STRING_LIST:
                col.list_offsets.reserve(num_rows);
                col.string_offsets.reserve(num_rows);
                col.string_data.reserve(num_rows * 16);
                break;
            case StorageKind::INT64_LIST:
                col.list_offsets.reserve(num_rows);
                col.int64_values.reserve(num_rows);
                break;
            case StorageKind::STRUCT_LIST:
                col.list_offsets.reserve(num_rows);
                break;
        }
        col.validity.reserve(num_rows);
    }
}

ArrowExportResult RecordBatchBuilder::finish() {
    const std::int64_t ncols = static_cast<std::int64_t>(columns_.size());
    const std::int64_t nrows = static_cast<std::int64_t>(num_rows_);

    nanoarrow::UniqueSchema schema;
    if (ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_STRUCT) !=
        NANOARROW_OK) {
        throw DFTUtilsException(ErrorCode::INTERNAL,
                                "ArrowSchemaInitFromType(STRUCT) failed");
    }
    if (ArrowSchemaAllocateChildren(schema.get(), ncols) != NANOARROW_OK) {
        throw DFTUtilsException(ErrorCode::INTERNAL,
                                "ArrowSchemaAllocateChildren failed");
    }
    for (std::int64_t i = 0; i < ncols; ++i) {
        const auto& col = columns_[static_cast<std::size_t>(i)];
        ArrowSchema* child_schema = schema->children[i];

        if (col.type == ColumnType::DICT_STRING) {
            // Dictionary-encoded string: indices are INT32, values are STRING
            if (ArrowSchemaInitFromType(child_schema, NANOARROW_TYPE_INT32) !=
                NANOARROW_OK) {
                throw DFTUtilsException(
                    ErrorCode::INTERNAL,
                    "ArrowSchemaInitFromType(dict indices) failed");
            }
            if (ArrowSchemaAllocateDictionary(child_schema) != NANOARROW_OK) {
                throw DFTUtilsException(ErrorCode::INTERNAL,
                                        "ArrowSchemaAllocateDictionary failed");
            }
            if (ArrowSchemaInitFromType(child_schema->dictionary,
                                        NANOARROW_TYPE_STRING) !=
                NANOARROW_OK) {
                throw DFTUtilsException(
                    ErrorCode::INTERNAL,
                    "ArrowSchemaInitFromType(dict values) failed");
            }
        } else if (col.type == ColumnType::HIST) {
            // list<struct<lo:double, hi:double, count:uint64>>
            if (ArrowSchemaInitFromType(child_schema, NANOARROW_TYPE_LIST) !=
                NANOARROW_OK) {
                throw DFTUtilsException(ErrorCode::INTERNAL,
                                        "ArrowSchemaInitFromType(list) failed");
            }
            // ArrowSchemaInitFromType(LIST) already allocated, initialized and
            // named children[0] "item"; re-initializing it here would orphan
            // that name allocation (ArrowSchemaInit nulls the field without
            // freeing). Set the type in place instead.
            ArrowSchema* item = child_schema->children[0];
            if (ArrowSchemaSetType(item, NANOARROW_TYPE_STRUCT) !=
                    NANOARROW_OK ||
                ArrowSchemaAllocateChildren(item, 3) != NANOARROW_OK) {
                throw DFTUtilsException(ErrorCode::INTERNAL,
                                        "hist struct schema init failed");
            }
            const ArrowType fts[3] = {NANOARROW_TYPE_DOUBLE,
                                      NANOARROW_TYPE_DOUBLE,
                                      NANOARROW_TYPE_UINT64};
            const char* fnames[3] = {"lo", "hi", "count"};
            for (int c = 0; c < 3; ++c) {
                if (ArrowSchemaInitFromType(item->children[c], fts[c]) !=
                        NANOARROW_OK ||
                    ArrowSchemaSetName(item->children[c], fnames[c]) !=
                        NANOARROW_OK) {
                    throw DFTUtilsException(ErrorCode::INTERNAL,
                                            "hist field schema init failed");
                }
            }
        } else if (col.type == ColumnType::STRING_LIST) {
            // Set item type in place; see the HIST note above.
            if (ArrowSchemaInitFromType(child_schema, NANOARROW_TYPE_LIST) !=
                NANOARROW_OK) {
                throw DFTUtilsException(ErrorCode::INTERNAL,
                                        "ArrowSchemaInitFromType(list) failed");
            }
            if (ArrowSchemaSetType(child_schema->children[0],
                                   NANOARROW_TYPE_STRING) != NANOARROW_OK) {
                throw DFTUtilsException(ErrorCode::INTERNAL,
                                        "string-list item schema init failed");
            }
        } else if (col.type == ColumnType::INT64_LIST) {
            // Set item type in place; see the HIST note above.
            if (ArrowSchemaInitFromType(child_schema, NANOARROW_TYPE_LIST) !=
                NANOARROW_OK) {
                throw DFTUtilsException(ErrorCode::INTERNAL,
                                        "ArrowSchemaInitFromType(list) failed");
            }
            if (ArrowSchemaSetType(child_schema->children[0],
                                   NANOARROW_TYPE_INT64) != NANOARROW_OK) {
                throw DFTUtilsException(ErrorCode::INTERNAL,
                                        "int64-list item schema init failed");
            }
        } else if (col.type == ColumnType::STRUCT_LIST) {
            // Set item type in place; see the HIST note above.
            if (ArrowSchemaInitFromType(child_schema, NANOARROW_TYPE_LIST) !=
                NANOARROW_OK) {
                throw DFTUtilsException(ErrorCode::INTERNAL,
                                        "ArrowSchemaInitFromType(list) failed");
            }
            ArrowSchema* item = child_schema->children[0];
            const std::int64_t nfields =
                static_cast<std::int64_t>(col.struct_fields.size());
            if (ArrowSchemaSetType(item, NANOARROW_TYPE_STRUCT) !=
                    NANOARROW_OK ||
                ArrowSchemaAllocateChildren(item, nfields) != NANOARROW_OK) {
                throw DFTUtilsException(ErrorCode::INTERNAL,
                                        "struct-list schema init failed");
            }
            for (std::int64_t f = 0; f < nfields; ++f) {
                const auto& fs = col.struct_fields[static_cast<std::size_t>(f)];
                if (ArrowSchemaInitFromType(item->children[f],
                                            to_nanoarrow_type(fs.type)) !=
                        NANOARROW_OK ||
                    ArrowSchemaSetName(item->children[f], fs.name.c_str()) !=
                        NANOARROW_OK) {
                    throw DFTUtilsException(
                        ErrorCode::INTERNAL,
                        "struct-list field schema init failed");
                }
            }
        } else {
            if (ArrowSchemaInitFromType(child_schema,
                                        to_nanoarrow_type(col.type)) !=
                NANOARROW_OK) {
                throw DFTUtilsException(
                    ErrorCode::INTERNAL,
                    "ArrowSchemaInitFromType(child) failed");
            }
        }
        if (ArrowSchemaSetName(child_schema, col.name.c_str()) !=
            NANOARROW_OK) {
            throw DFTUtilsException(ErrorCode::INTERNAL,
                                    "ArrowSchemaSetName failed");
        }
    }

    nanoarrow::UniqueArray array;
    if (ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr) !=
        NANOARROW_OK) {
        throw DFTUtilsException(ErrorCode::INTERNAL,
                                "ArrowArrayInitFromSchema failed");
    }
    // StartAppending initialises children recursively.
    if (ArrowArrayStartAppending(array.get()) != NANOARROW_OK) {
        throw DFTUtilsException(ErrorCode::INTERNAL,
                                "ArrowArrayStartAppending failed");
    }

    for (std::int64_t i = 0; i < ncols; ++i) {
        const auto& col = columns_[static_cast<std::size_t>(i)];
        ArrowArray* child = array->children[i];

        if (ArrowArrayReserve(child, nrows) != NANOARROW_OK) {
            throw DFTUtilsException(ErrorCode::INTERNAL,
                                    "ArrowArrayReserve failed");
        }

        const std::size_t row_count =
            std::min<std::size_t>(col.count, static_cast<std::size_t>(nrows));
        std::int64_t null_count = 0;

        auto fill_validity = [&]() {
            if (!col.has_nulls) return;
            ArrowBitmap* bm = ArrowArrayValidityBitmap(child);
            ArrowBitmapReserve(bm, static_cast<std::int64_t>(row_count));
            for (std::size_t r = 0; r < row_count; ++r) {
                std::uint8_t v = col.validity[r];
                if (!v) ++null_count;
                ArrowBitmapAppendUnsafe(bm, v, 1);
            }
        };

        switch (col.type) {
            case ColumnType::INT64: {
                fill_validity();
                ArrowBuffer* data_buf = ArrowArrayBuffer(child, 1);
                if (ArrowBufferAppend(data_buf, col.int64_values.data(),
                                      static_cast<std::int64_t>(
                                          row_count * sizeof(std::int64_t))) !=
                    NANOARROW_OK) {
                    throw DFTUtilsException(ErrorCode::INTERNAL,
                                            "INT64 buffer append failed");
                }
                break;
            }
            case ColumnType::UINT64: {
                fill_validity();
                ArrowBuffer* data_buf = ArrowArrayBuffer(child, 1);
                if (ArrowBufferAppend(data_buf, col.uint64_values.data(),
                                      static_cast<std::int64_t>(
                                          row_count * sizeof(std::uint64_t))) !=
                    NANOARROW_OK) {
                    throw DFTUtilsException(ErrorCode::INTERNAL,
                                            "UINT64 buffer append failed");
                }
                break;
            }
            case ColumnType::DOUBLE: {
                fill_validity();
                ArrowBuffer* data_buf = ArrowArrayBuffer(child, 1);
                if (ArrowBufferAppend(data_buf, col.double_values.data(),
                                      static_cast<std::int64_t>(
                                          row_count * sizeof(double))) !=
                    NANOARROW_OK) {
                    throw DFTUtilsException(ErrorCode::INTERNAL,
                                            "DOUBLE buffer append failed");
                }
                break;
            }
            case ColumnType::STRING:
            case ColumnType::BINARY: {
                // STRING and BINARY have identical offset+data buffer layout.
                fill_validity();
                ArrowBuffer* offsets_buf = ArrowArrayBuffer(child, 1);
                ArrowBuffer* data_buf = ArrowArrayBuffer(child, 2);
                if (row_count > 0) {
                    ArrowBufferReserve(offsets_buf,
                                       row_count * sizeof(std::int32_t));
                    ArrowBufferAppend(offsets_buf, col.string_offsets.data(),
                                      static_cast<std::int64_t>(
                                          row_count * sizeof(std::int32_t)));
                }
                if (!col.string_data.empty()) {
                    ArrowBufferAppend(
                        data_buf, col.string_data.data(),
                        static_cast<std::int64_t>(col.string_data.size()));
                }
                break;
            }
            case ColumnType::BOOL: {
                for (std::size_t r = 0; r < row_count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "ArrowArrayAppendNull(bool) failed");
                        }
                        ++null_count;
                    } else {
                        if (ArrowArrayAppendInt(child, col.bool_values[r]) !=
                            NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "ArrowArrayAppendInt(bool) failed");
                        }
                    }
                }
                break;
            }
            case ColumnType::DICT_STRING: {
                for (std::size_t r = 0; r < col.count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "ArrowArrayAppendNull(dict) failed");
                        }
                    } else {
                        if (ArrowArrayAppendInt(child, col.dict_indices[r]) !=
                            NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "ArrowArrayAppendInt(dict index) failed");
                        }
                    }
                }

                if (child->dictionary != nullptr) {
                    if (child->dictionary->release != nullptr) {
                        ArrowArrayRelease(child->dictionary);
                    }
                    ArrowFree(child->dictionary);
                    child->dictionary = nullptr;
                }
                child->dictionary =
                    static_cast<ArrowArray*>(ArrowMalloc(sizeof(ArrowArray)));
                if (!child->dictionary) {
                    throw DFTUtilsException(ErrorCode::INTERNAL,
                                            "Failed to allocate dictionary");
                }
                ArrowArrayInitFromType(child->dictionary,
                                       NANOARROW_TYPE_STRING);
                if (ArrowArrayStartAppending(child->dictionary) !=
                    NANOARROW_OK) {
                    throw DFTUtilsException(
                        ErrorCode::INTERNAL,
                        "ArrowArrayStartAppending(dict) failed");
                }
                if (ArrowArrayReserve(
                        child->dictionary,
                        static_cast<std::int64_t>(col.dict_values.size())) !=
                    NANOARROW_OK) {
                    throw DFTUtilsException(ErrorCode::INTERNAL,
                                            "ArrowArrayReserve(dict) failed");
                }
                for (const auto& s : col.dict_values) {
                    ArrowStringView asv{s.data(),
                                        static_cast<std::int64_t>(s.size())};
                    if (ArrowArrayAppendString(child->dictionary, asv) !=
                        NANOARROW_OK) {
                        throw DFTUtilsException(
                            ErrorCode::INTERNAL,
                            "ArrowArrayAppendString(dict) failed");
                    }
                }
                if (ArrowArrayFinishBuildingDefault(child->dictionary,
                                                    nullptr) != NANOARROW_OK) {
                    throw DFTUtilsException(
                        ErrorCode::INTERNAL,
                        "ArrowArrayFinishBuildingDefault(dict) failed");
                }
                break;
            }
            case ColumnType::HIST: {
                ArrowArray* st = child->children[0];
                ArrowArray* c_lo = st->children[0];
                ArrowArray* c_hi = st->children[1];
                ArrowArray* c_cnt = st->children[2];
                std::size_t pos = 0;
                for (std::size_t r = 0; r < row_count; ++r) {
                    const std::int32_t end = col.hist_offsets[r];
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "ArrowArrayAppendNull(hist) failed");
                        }
                        ++null_count;
                        pos = static_cast<std::size_t>(end);
                        continue;
                    }
                    for (; pos < static_cast<std::size_t>(end); ++pos) {
                        const auto& b = col.hist_bins[pos];
                        if (ArrowArrayAppendDouble(c_lo, b.lower) !=
                                NANOARROW_OK ||
                            ArrowArrayAppendDouble(c_hi, b.upper) !=
                                NANOARROW_OK ||
                            ArrowArrayAppendUInt(c_cnt, b.count) !=
                                NANOARROW_OK ||
                            ArrowArrayFinishElement(st) != NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "hist bucket append failed");
                        }
                    }
                    if (ArrowArrayFinishElement(child) != NANOARROW_OK) {
                        throw DFTUtilsException(
                            ErrorCode::INTERNAL,
                            "ArrowArrayFinishElement(hist) failed");
                    }
                }
                break;
            }
            case ColumnType::STRING_LIST: {
                ArrowArray* item = child->children[0];
                std::size_t pos = 0;
                for (std::size_t r = 0; r < row_count; ++r) {
                    const std::int32_t end = col.list_offsets[r];
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "ArrowArrayAppendNull(string-list) failed");
                        }
                        ++null_count;
                        pos = static_cast<std::size_t>(end);
                        continue;
                    }
                    for (; pos < static_cast<std::size_t>(end); ++pos) {
                        const std::int32_t begin =
                            pos == 0 ? 0 : col.string_offsets[pos - 1];
                        ArrowStringView asv{col.string_data.data() + begin,
                                            col.string_offsets[pos] - begin};
                        if (ArrowArrayAppendString(item, asv) != NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "ArrowArrayAppendString(string-list) failed");
                        }
                    }
                    if (ArrowArrayFinishElement(child) != NANOARROW_OK) {
                        throw DFTUtilsException(
                            ErrorCode::INTERNAL,
                            "ArrowArrayFinishElement(string-list) failed");
                    }
                }
                break;
            }
            case ColumnType::INT64_LIST: {
                ArrowArray* item = child->children[0];
                std::size_t pos = 0;
                for (std::size_t r = 0; r < row_count; ++r) {
                    const std::int32_t end = col.list_offsets[r];
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "ArrowArrayAppendNull(int64-list) failed");
                        }
                        ++null_count;
                        pos = static_cast<std::size_t>(end);
                        continue;
                    }
                    for (; pos < static_cast<std::size_t>(end); ++pos) {
                        if (ArrowArrayAppendInt(item, col.int64_values[pos]) !=
                            NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "ArrowArrayAppendInt(int64-list) failed");
                        }
                    }
                    if (ArrowArrayFinishElement(child) != NANOARROW_OK) {
                        throw DFTUtilsException(
                            ErrorCode::INTERNAL,
                            "ArrowArrayFinishElement(int64-list) failed");
                    }
                }
                break;
            }
            case ColumnType::STRUCT_LIST: {
                ArrowArray* st = child->children[0];
                const std::size_t nfields = col.struct_children.size();
                std::size_t pos = 0;
                for (std::size_t r = 0; r < row_count; ++r) {
                    const std::int32_t end = col.list_offsets[r];
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "ArrowArrayAppendNull(struct-list) failed");
                        }
                        ++null_count;
                        pos = static_cast<std::size_t>(end);
                        continue;
                    }
                    for (; pos < static_cast<std::size_t>(end); ++pos) {
                        for (std::size_t f = 0; f < nfields; ++f) {
                            const ColumnData& fc = col.struct_children[f];
                            ArrowArray* cf = st->children[f];
                            bool ok = true;
                            switch (fc.type) {
                                case ColumnType::DOUBLE:
                                case ColumnType::FLOAT32:
                                    ok = ArrowArrayAppendDouble(
                                             cf, fc.double_values[pos]) ==
                                         NANOARROW_OK;
                                    break;
                                case ColumnType::UINT64:
                                case ColumnType::UINT8:
                                case ColumnType::UINT16:
                                case ColumnType::UINT32:
                                    ok = ArrowArrayAppendUInt(
                                             cf, fc.uint64_values[pos]) ==
                                         NANOARROW_OK;
                                    break;
                                case ColumnType::STRING: {
                                    const std::int32_t begin =
                                        pos == 0 ? 0
                                                 : fc.string_offsets[pos - 1];
                                    ArrowStringView asv{
                                        fc.string_data.data() + begin,
                                        fc.string_offsets[pos] - begin};
                                    ok = ArrowArrayAppendString(cf, asv) ==
                                         NANOARROW_OK;
                                    break;
                                }
                                case ColumnType::BINARY: {
                                    const std::int32_t begin =
                                        pos == 0 ? 0
                                                 : fc.string_offsets[pos - 1];
                                    ArrowBufferView bv;
                                    bv.data.as_char =
                                        fc.string_data.data() + begin;
                                    bv.size_bytes =
                                        fc.string_offsets[pos] - begin;
                                    ok = ArrowArrayAppendBytes(cf, bv) ==
                                         NANOARROW_OK;
                                    break;
                                }
                                default:
                                    ok = ArrowArrayAppendInt(
                                             cf, fc.int64_values[pos]) ==
                                         NANOARROW_OK;
                                    break;
                            }
                            if (!ok) {
                                throw DFTUtilsException(
                                    ErrorCode::INTERNAL,
                                    "struct-list field append failed");
                            }
                        }
                        if (ArrowArrayFinishElement(st) != NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "ArrowArrayFinishElement(struct) failed");
                        }
                    }
                    if (ArrowArrayFinishElement(child) != NANOARROW_OK) {
                        throw DFTUtilsException(
                            ErrorCode::INTERNAL,
                            "ArrowArrayFinishElement(struct-list) failed");
                    }
                }
                break;
            }
            // Narrow widths append element-wise so nanoarrow writes the exact
            // Arrow width; the bulk path above assumes 8-byte cells.
            case ColumnType::INT8:
            case ColumnType::INT16:
            case ColumnType::INT32: {
                for (std::size_t r = 0; r < row_count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "ArrowArrayAppendNull(int) failed");
                        }
                    } else if (ArrowArrayAppendInt(child,
                                                   col.int64_values[r]) !=
                               NANOARROW_OK) {
                        throw DFTUtilsException(ErrorCode::INTERNAL,
                                                "ArrowArrayAppendInt failed");
                    }
                }
                break;
            }
            case ColumnType::UINT8:
            case ColumnType::UINT16:
            case ColumnType::UINT32: {
                for (std::size_t r = 0; r < row_count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "ArrowArrayAppendNull(uint) failed");
                        }
                    } else if (ArrowArrayAppendUInt(child,
                                                    col.uint64_values[r]) !=
                               NANOARROW_OK) {
                        throw DFTUtilsException(ErrorCode::INTERNAL,
                                                "ArrowArrayAppendUInt failed");
                    }
                }
                break;
            }
            case ColumnType::FLOAT32: {
                for (std::size_t r = 0; r < row_count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw DFTUtilsException(
                                ErrorCode::INTERNAL,
                                "ArrowArrayAppendNull(float32) failed");
                        }
                    } else if (ArrowArrayAppendDouble(child,
                                                      col.double_values[r]) !=
                               NANOARROW_OK) {
                        throw DFTUtilsException(
                            ErrorCode::INTERNAL,
                            "ArrowArrayAppendDouble(float32) failed");
                    }
                }
                break;
            }
        }

        if (col.type == ColumnType::INT64 || col.type == ColumnType::UINT64 ||
            col.type == ColumnType::DOUBLE || col.type == ColumnType::STRING ||
            col.type == ColumnType::BINARY) {
            child->length = static_cast<std::int64_t>(row_count);
            child->null_count = col.has_nulls ? null_count : 0;
        }

        if (ArrowArrayFinishBuildingDefault(child, nullptr) != NANOARROW_OK) {
            throw DFTUtilsException(
                ErrorCode::INTERNAL,
                "ArrowArrayFinishBuildingDefault(child) failed");
        }
    }

    array->length = nrows;
    array->null_count = 0;

    if (ArrowArrayFinishBuildingDefault(array.get(), nullptr) != NANOARROW_OK) {
        throw DFTUtilsException(
            ErrorCode::INTERNAL,
            "ArrowArrayFinishBuildingDefault(struct) failed");
    }

    return ArrowExportResult(std::move(schema), std::move(array));
}

void RecordBatchBuilder::reset(bool keep_schema) {
    if (keep_schema && (schema_declared_ || schema_locked_)) {
        for (auto& col : columns_) {
            col.int64_values.clear();
            col.uint64_values.clear();
            col.double_values.clear();
            col.string_offsets.clear();
            col.string_data.clear();
            col.bool_values.clear();
            col.dict_indices.clear();
            col.dict_values.clear();
            col.dict_map.clear();
            col.hist_bins.clear();
            col.hist_offsets.clear();
            col.list_offsets.clear();
            col.validity.clear();
            col.count = 0;
            col.has_nulls = false;
            for (auto& fc : col.struct_children) {
                fc.int64_values.clear();
                fc.uint64_values.clear();
                fc.double_values.clear();
                fc.string_offsets.clear();
                fc.string_data.clear();
                fc.count = 0;
            }
        }
        // Reset touched flags but keep the vector size
        std::fill(touched_.begin(), touched_.end(), false);
    } else {
        columns_.clear();
        name_to_index_.clear();
        touched_.clear();
        schema_declared_ = false;
        schema_locked_ = false;
    }
    num_rows_ = 0;
}

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
