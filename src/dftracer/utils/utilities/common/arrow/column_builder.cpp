#include <dftracer/utils/core/common/config.h>
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
        case ColumnType::BOOL:
            return NANOARROW_TYPE_BOOL;
        case ColumnType::DICT_STRING:
            // Dictionary uses INT32 indices; dictionary values handled
            // separately
            return NANOARROW_TYPE_INT32;
    }
    return NANOARROW_TYPE_UNINITIALIZED;
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

    switch (col.type) {
        case ColumnType::INT64:
            col.int64_values.resize(col.count + n, 0);
            break;
        case ColumnType::UINT64:
            col.uint64_values.resize(col.count + n, 0);
            break;
        case ColumnType::DOUBLE:
            col.double_values.resize(col.count + n, 0.0);
            break;
        case ColumnType::STRING:
            col.string_offsets.resize(
                col.count + n,
                static_cast<std::int32_t>(col.string_data.size()));
            break;
        case ColumnType::BOOL:
            col.bool_values.resize(col.count + n, 0);
            break;
        case ColumnType::DICT_STRING:
            col.dict_indices.resize(col.count + n, -1);  // -1 = null
            break;
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

void RecordBatchBuilder::append_int64(std::size_t col_idx, std::int64_t value) {
    auto& col = columns_[col_idx];
    col.int64_values.push_back(value);
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    if (!schema_declared_ && !schema_locked_ && !touched_[col_idx]) {
        touched_[col_idx] = true;
        ++row_touched_count_;
    }
}

void RecordBatchBuilder::append_uint64(std::size_t col_idx,
                                       std::uint64_t value) {
    auto& col = columns_[col_idx];
    col.uint64_values.push_back(value);
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    if (!schema_declared_ && !schema_locked_ && !touched_[col_idx]) {
        touched_[col_idx] = true;
        ++row_touched_count_;
    }
}

void RecordBatchBuilder::append_double(std::size_t col_idx, double value) {
    auto& col = columns_[col_idx];
    col.double_values.push_back(value);
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    if (!schema_declared_ && !schema_locked_ && !touched_[col_idx]) {
        touched_[col_idx] = true;
        ++row_touched_count_;
    }
}

void RecordBatchBuilder::append_string(std::size_t col_idx,
                                       std::string_view value) {
    auto& col = columns_[col_idx];
    col.string_data.insert(col.string_data.end(), value.begin(), value.end());
    col.string_offsets.push_back(
        static_cast<std::int32_t>(col.string_data.size()));
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    if (!schema_declared_ && !schema_locked_ && !touched_[col_idx]) {
        touched_[col_idx] = true;
        ++row_touched_count_;
    }
}

void RecordBatchBuilder::append_dict_string(std::size_t col_idx,
                                            std::string_view value) {
    auto& col = columns_[col_idx];
    // Look up or insert into dictionary
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
    if (!schema_declared_ && !schema_locked_ && !touched_[col_idx]) {
        touched_[col_idx] = true;
        ++row_touched_count_;
    }
}

void RecordBatchBuilder::append_bool(std::size_t col_idx, bool value) {
    auto& col = columns_[col_idx];
    col.bool_values.push_back(value ? 1 : 0);
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    if (!schema_declared_ && !schema_locked_ && !touched_[col_idx]) {
        touched_[col_idx] = true;
        ++row_touched_count_;
    }
}

void RecordBatchBuilder::append_null(std::size_t col_idx) {
    auto& col = columns_[col_idx];
    if (!col.has_nulls) {
        col.has_nulls = true;
        col.validity.assign(col.count, 1);
    }
    col.validity.push_back(0);

    switch (col.type) {
        case ColumnType::INT64:
            col.int64_values.push_back(0);
            break;
        case ColumnType::UINT64:
            col.uint64_values.push_back(0);
            break;
        case ColumnType::DOUBLE:
            col.double_values.push_back(0.0);
            break;
        case ColumnType::STRING:
            col.string_offsets.push_back(
                static_cast<std::int32_t>(col.string_data.size()));
            break;
        case ColumnType::BOOL:
            col.bool_values.push_back(0);
            break;
        case ColumnType::DICT_STRING:
            col.dict_indices.push_back(-1);  // -1 = null
            break;
    }
    ++col.count;
    if (!schema_declared_ && !touched_[col_idx]) {
        touched_[col_idx] = true;
        ++row_touched_count_;
    }
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
        switch (col.type) {
            case ColumnType::INT64:
                col.int64_values.reserve(num_rows);
                break;
            case ColumnType::UINT64:
                col.uint64_values.reserve(num_rows);
                break;
            case ColumnType::DOUBLE:
                col.double_values.reserve(num_rows);
                break;
            case ColumnType::STRING:
                col.string_offsets.reserve(num_rows + 1);
                // dftracer hash strings are 16 bytes; common strings
                // (event names, categories) range 4-32. Bumping the
                // initial reservation cuts geometric-growth memmove churn
                // visible in perf for moderate batch sizes.
                col.string_data.reserve(num_rows * 32);
                break;
            case ColumnType::BOOL:
                col.bool_values.reserve(num_rows);
                break;
            case ColumnType::DICT_STRING:
                col.dict_indices.reserve(num_rows);
                break;
        }
        col.validity.reserve(num_rows);
    }
}

ArrowExportResult RecordBatchBuilder::finish() {
    const std::int64_t ncols = static_cast<std::int64_t>(columns_.size());
    const std::int64_t nrows = static_cast<std::int64_t>(num_rows_);

    // Build schema: struct with one child per column.
    nanoarrow::UniqueSchema schema;
    if (ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_STRUCT) !=
        NANOARROW_OK) {
        throw std::runtime_error("ArrowSchemaInitFromType(STRUCT) failed");
    }
    if (ArrowSchemaAllocateChildren(schema.get(), ncols) != NANOARROW_OK) {
        throw std::runtime_error("ArrowSchemaAllocateChildren failed");
    }
    for (std::int64_t i = 0; i < ncols; ++i) {
        const auto& col = columns_[static_cast<std::size_t>(i)];
        ArrowSchema* child_schema = schema->children[i];

        if (col.type == ColumnType::DICT_STRING) {
            // Dictionary-encoded string: indices are INT32, values are STRING
            if (ArrowSchemaInitFromType(child_schema, NANOARROW_TYPE_INT32) !=
                NANOARROW_OK) {
                throw std::runtime_error(
                    "ArrowSchemaInitFromType(dict indices) failed");
            }
            if (ArrowSchemaAllocateDictionary(child_schema) != NANOARROW_OK) {
                throw std::runtime_error(
                    "ArrowSchemaAllocateDictionary failed");
            }
            if (ArrowSchemaInitFromType(child_schema->dictionary,
                                        NANOARROW_TYPE_STRING) !=
                NANOARROW_OK) {
                throw std::runtime_error(
                    "ArrowSchemaInitFromType(dict values) failed");
            }
        } else {
            if (ArrowSchemaInitFromType(child_schema,
                                        to_nanoarrow_type(col.type)) !=
                NANOARROW_OK) {
                throw std::runtime_error(
                    "ArrowSchemaInitFromType(child) failed");
            }
        }
        if (ArrowSchemaSetName(child_schema, col.name.c_str()) !=
            NANOARROW_OK) {
            throw std::runtime_error("ArrowSchemaSetName failed");
        }
    }

    // Build struct array from schema.
    nanoarrow::UniqueArray array;
    if (ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr) !=
        NANOARROW_OK) {
        throw std::runtime_error("ArrowArrayInitFromSchema failed");
    }
    // StartAppending initialises children recursively.
    if (ArrowArrayStartAppending(array.get()) != NANOARROW_OK) {
        throw std::runtime_error("ArrowArrayStartAppending failed");
    }

    for (std::int64_t i = 0; i < ncols; ++i) {
        const auto& col = columns_[static_cast<std::size_t>(i)];
        ArrowArray* child = array->children[i];

        if (ArrowArrayReserve(child, nrows) != NANOARROW_OK) {
            throw std::runtime_error("ArrowArrayReserve failed");
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
                    throw std::runtime_error("INT64 buffer append failed");
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
                    throw std::runtime_error("UINT64 buffer append failed");
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
                    throw std::runtime_error("DOUBLE buffer append failed");
                }
                break;
            }
            case ColumnType::STRING: {
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
                            throw std::runtime_error(
                                "ArrowArrayAppendNull(bool) failed");
                        }
                        ++null_count;
                    } else {
                        if (ArrowArrayAppendInt(child, col.bool_values[r]) !=
                            NANOARROW_OK) {
                            throw std::runtime_error(
                                "ArrowArrayAppendInt(bool) failed");
                        }
                    }
                }
                break;
            }
            case ColumnType::DICT_STRING: {
                // Build indices array (INT32)
                for (std::size_t r = 0; r < col.count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw std::runtime_error(
                                "ArrowArrayAppendNull(dict) failed");
                        }
                    } else {
                        if (ArrowArrayAppendInt(child, col.dict_indices[r]) !=
                            NANOARROW_OK) {
                            throw std::runtime_error(
                                "ArrowArrayAppendInt(dict index) failed");
                        }
                    }
                }

                // Build dictionary array (STRING)
                // Allocate dictionary array
                child->dictionary =
                    static_cast<ArrowArray*>(ArrowMalloc(sizeof(ArrowArray)));
                if (!child->dictionary) {
                    throw std::runtime_error("Failed to allocate dictionary");
                }
                ArrowArrayInitFromType(child->dictionary,
                                       NANOARROW_TYPE_STRING);
                if (ArrowArrayStartAppending(child->dictionary) !=
                    NANOARROW_OK) {
                    throw std::runtime_error(
                        "ArrowArrayStartAppending(dict) failed");
                }
                if (ArrowArrayReserve(
                        child->dictionary,
                        static_cast<std::int64_t>(col.dict_values.size())) !=
                    NANOARROW_OK) {
                    throw std::runtime_error("ArrowArrayReserve(dict) failed");
                }
                for (const auto& s : col.dict_values) {
                    ArrowStringView asv{s.data(),
                                        static_cast<std::int64_t>(s.size())};
                    if (ArrowArrayAppendString(child->dictionary, asv) !=
                        NANOARROW_OK) {
                        throw std::runtime_error(
                            "ArrowArrayAppendString(dict) failed");
                    }
                }
                if (ArrowArrayFinishBuildingDefault(child->dictionary,
                                                    nullptr) != NANOARROW_OK) {
                    throw std::runtime_error(
                        "ArrowArrayFinishBuildingDefault(dict) failed");
                }
                break;
            }
        }

        if (col.type == ColumnType::INT64 || col.type == ColumnType::UINT64 ||
            col.type == ColumnType::DOUBLE || col.type == ColumnType::STRING) {
            child->length = static_cast<std::int64_t>(row_count);
            child->null_count = col.has_nulls ? null_count : 0;
        }

        if (ArrowArrayFinishBuildingDefault(child, nullptr) != NANOARROW_OK) {
            throw std::runtime_error(
                "ArrowArrayFinishBuildingDefault(child) failed");
        }
    }

    array->length = nrows;
    array->null_count = 0;

    if (ArrowArrayFinishBuildingDefault(array.get(), nullptr) != NANOARROW_OK) {
        throw std::runtime_error(
            "ArrowArrayFinishBuildingDefault(struct) failed");
    }

    return ArrowExportResult(std::move(schema), std::move(array));
}

void RecordBatchBuilder::reset(bool keep_schema) {
    // Keep schema if explicitly declared OR if dynamically locked
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
            col.validity.clear();
            col.count = 0;
            col.has_nulls = false;
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
