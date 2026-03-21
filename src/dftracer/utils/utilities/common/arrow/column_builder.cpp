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

void RecordBatchBuilder::backfill_nulls(ColumnData& col, size_t target_count) {
    size_t n = target_count - col.count;
    if (n == 0) return;

    col.has_nulls = true;
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
            col.string_values.resize(col.count + n, std::string_view{});
            break;
        case ColumnType::BOOL:
            col.bool_values.resize(col.count + n, 0);
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
        size_t idx = columns_.size();
        columns_.emplace_back();
        init_column(columns_.back(), spec.type, spec.name);
        name_to_index_[spec.name] = idx;
    }
    schema_declared_ = true;
}

size_t RecordBatchBuilder::add_or_get_column(std::string_view name,
                                             ColumnType type) {
    auto it = name_to_index_.find(std::string(name));
    if (it != name_to_index_.end()) {
        // Existing column: type is ignored. Callers that need type-safe
        // appends should use find_column() + column_type() first.
        return it->second;
    }

    size_t idx = columns_.size();
    columns_.emplace_back();
    init_column(columns_.back(), type, name);
    if (num_rows_ > 0) {
        backfill_nulls(columns_.back(), num_rows_);
    }
    name_to_index_[std::string(name)] = idx;
    touched_.push_back(false);
    return idx;
}

std::optional<size_t> RecordBatchBuilder::find_column(
    std::string_view name) const {
    auto it = name_to_index_.find(std::string(name));
    if (it != name_to_index_.end()) return it->second;
    return std::nullopt;
}

ColumnType RecordBatchBuilder::column_type(size_t col_idx) const noexcept {
    return columns_[col_idx].type;
}

void RecordBatchBuilder::append_int64(size_t col_idx, int64_t value) {
    auto& col = columns_[col_idx];
    col.int64_values.push_back(value);
    col.validity.push_back(1);
    ++col.count;
    if (!schema_declared_) touched_[col_idx] = true;
}

void RecordBatchBuilder::append_uint64(size_t col_idx, uint64_t value) {
    auto& col = columns_[col_idx];
    col.uint64_values.push_back(value);
    col.validity.push_back(1);
    ++col.count;
    if (!schema_declared_) touched_[col_idx] = true;
}

void RecordBatchBuilder::append_double(size_t col_idx, double value) {
    auto& col = columns_[col_idx];
    col.double_values.push_back(value);
    col.validity.push_back(1);
    ++col.count;
    if (!schema_declared_) touched_[col_idx] = true;
}

void RecordBatchBuilder::append_string(size_t col_idx, std::string_view value) {
    auto& col = columns_[col_idx];
    col.string_values.push_back(value);
    col.validity.push_back(1);
    ++col.count;
    if (!schema_declared_) touched_[col_idx] = true;
}

void RecordBatchBuilder::append_bool(size_t col_idx, bool value) {
    auto& col = columns_[col_idx];
    col.bool_values.push_back(value ? 1 : 0);
    col.validity.push_back(1);
    ++col.count;
    if (!schema_declared_) touched_[col_idx] = true;
}

void RecordBatchBuilder::append_null(size_t col_idx) {
    auto& col = columns_[col_idx];
    col.has_nulls = true;
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
            col.string_values.push_back(std::string_view{});
            break;
        case ColumnType::BOOL:
            col.bool_values.push_back(0);
            break;
    }
    ++col.count;
    if (!schema_declared_) touched_[col_idx] = true;
}

void RecordBatchBuilder::end_row() {
    // Backfill nulls for any column not appended to this row.
    // In dynamic mode, use touched_ flags; in static mode, compare counts.
    for (size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].count <= num_rows_) {
            backfill_nulls(columns_[i], num_rows_ + 1);
        }
        if (!schema_declared_) touched_[i] = false;
    }
    ++num_rows_;
}

void RecordBatchBuilder::reserve(size_t num_rows) {
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
                col.string_values.reserve(num_rows);
                break;
            case ColumnType::BOOL:
                col.bool_values.reserve(num_rows);
                break;
        }
        col.validity.reserve(num_rows);
    }
}

ArrowExportResult RecordBatchBuilder::finish() {
    const int64_t ncols = static_cast<int64_t>(columns_.size());
    const int64_t nrows = static_cast<int64_t>(num_rows_);

    // Build schema: struct with one child per column.
    nanoarrow::UniqueSchema schema;
    if (ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_STRUCT) !=
        NANOARROW_OK) {
        throw std::runtime_error("ArrowSchemaInitFromType(STRUCT) failed");
    }
    if (ArrowSchemaAllocateChildren(schema.get(), ncols) != NANOARROW_OK) {
        throw std::runtime_error("ArrowSchemaAllocateChildren failed");
    }
    for (int64_t i = 0; i < ncols; ++i) {
        const auto& col = columns_[static_cast<size_t>(i)];
        ArrowSchema* child_schema = schema->children[i];
        if (ArrowSchemaInitFromType(
                child_schema, to_nanoarrow_type(col.type)) != NANOARROW_OK) {
            throw std::runtime_error("ArrowSchemaInitFromType(child) failed");
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

    for (int64_t i = 0; i < ncols; ++i) {
        const auto& col = columns_[static_cast<size_t>(i)];
        ArrowArray* child = array->children[i];

        if (ArrowArrayReserve(child, nrows) != NANOARROW_OK) {
            throw std::runtime_error("ArrowArrayReserve failed");
        }

        // AppendNull handles validity bits internally.
        switch (col.type) {
            case ColumnType::INT64:
                for (size_t r = 0; r < col.count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw std::runtime_error(
                                "ArrowArrayAppendNull failed");
                        }
                    } else {
                        if (ArrowArrayAppendInt(child, col.int64_values[r]) !=
                            NANOARROW_OK) {
                            throw std::runtime_error(
                                "ArrowArrayAppendInt failed");
                        }
                    }
                }
                break;
            case ColumnType::UINT64:
                for (size_t r = 0; r < col.count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw std::runtime_error(
                                "ArrowArrayAppendNull failed");
                        }
                    } else {
                        if (ArrowArrayAppendUInt(child, col.uint64_values[r]) !=
                            NANOARROW_OK) {
                            throw std::runtime_error(
                                "ArrowArrayAppendUInt failed");
                        }
                    }
                }
                break;
            case ColumnType::DOUBLE:
                for (size_t r = 0; r < col.count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw std::runtime_error(
                                "ArrowArrayAppendNull failed");
                        }
                    } else {
                        if (ArrowArrayAppendDouble(
                                child, col.double_values[r]) != NANOARROW_OK) {
                            throw std::runtime_error(
                                "ArrowArrayAppendDouble failed");
                        }
                    }
                }
                break;
            case ColumnType::STRING: {
                for (size_t r = 0; r < col.count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw std::runtime_error(
                                "ArrowArrayAppendNull failed");
                        }
                    } else {
                        std::string_view sv = col.string_values[r];
                        ArrowStringView asv{sv.data(),
                                            static_cast<int64_t>(sv.size())};
                        if (ArrowArrayAppendString(child, asv) !=
                            NANOARROW_OK) {
                            throw std::runtime_error(
                                "ArrowArrayAppendString failed");
                        }
                    }
                }
                break;
            }
            case ColumnType::BOOL:
                for (size_t r = 0; r < col.count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            throw std::runtime_error(
                                "ArrowArrayAppendNull failed");
                        }
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
    if (keep_schema && schema_declared_) {
        for (auto& col : columns_) {
            col.int64_values.clear();
            col.uint64_values.clear();
            col.double_values.clear();
            col.string_values.clear();
            col.bool_values.clear();
            col.validity.clear();
            col.count = 0;
            col.has_nulls = false;
        }
    } else {
        columns_.clear();
        name_to_index_.clear();
        touched_.clear();
        schema_declared_ = false;
    }
    num_rows_ = 0;
}

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
