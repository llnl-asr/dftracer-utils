// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/arrow/column_builder.h>
#include <nanoarrow/nanoarrow.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace dftracer::utils::utilities::arrow {

namespace {

ArrowType to_nanoarrow_type(ColumnType t) noexcept {
    switch (t) {
        case ColumnType::Int64:
            return NANOARROW_TYPE_INT64;
        case ColumnType::Uint64:
            return NANOARROW_TYPE_UINT64;
        case ColumnType::Double:
            return NANOARROW_TYPE_DOUBLE;
        case ColumnType::String:
            return NANOARROW_TYPE_STRING;
        case ColumnType::Binary:
            return NANOARROW_TYPE_BINARY;
        case ColumnType::Bool:
            return NANOARROW_TYPE_BOOL;
        case ColumnType::DictString:
            // Dictionary uses INT32 indices; dictionary values handled
            // separately
            return NANOARROW_TYPE_INT32;
        case ColumnType::Hist:
            return NANOARROW_TYPE_LIST;
        case ColumnType::StringList:
            return NANOARROW_TYPE_LIST;
        case ColumnType::Int64List:
            return NANOARROW_TYPE_LIST;
        case ColumnType::StructList:
            return NANOARROW_TYPE_LIST;
        case ColumnType::Int8:
            return NANOARROW_TYPE_INT8;
        case ColumnType::Int16:
            return NANOARROW_TYPE_INT16;
        case ColumnType::Int32:
            return NANOARROW_TYPE_INT32;
        case ColumnType::Uint8:
            return NANOARROW_TYPE_UINT8;
        case ColumnType::Uint16:
            return NANOARROW_TYPE_UINT16;
        case ColumnType::Uint32:
            return NANOARROW_TYPE_UINT32;
        case ColumnType::Float32:
            return NANOARROW_TYPE_FLOAT;
    }
    return NANOARROW_TYPE_UNINITIALIZED;
}

constexpr StorageKind storage_kind(ColumnType t) noexcept {
    switch (t) {
        case ColumnType::Int64:
        case ColumnType::Int8:
        case ColumnType::Int16:
        case ColumnType::Int32:
            return StorageKind::Int64;
        case ColumnType::Uint64:
        case ColumnType::Uint8:
        case ColumnType::Uint16:
        case ColumnType::Uint32:
            return StorageKind::Uint64;
        case ColumnType::Double:
        case ColumnType::Float32:
            return StorageKind::Double;
        case ColumnType::String:
        case ColumnType::Binary:
            return StorageKind::String;
        case ColumnType::Bool:
            return StorageKind::Bool;
        case ColumnType::DictString:
            return StorageKind::Dict;
        case ColumnType::Hist:
            return StorageKind::Hist;
        case ColumnType::StringList:
            return StorageKind::StringList;
        case ColumnType::Int64List:
            return StorageKind::Int64List;
        case ColumnType::StructList:
            return StorageKind::StructList;
    }
    return StorageKind::Int64;
}

bool is_number(ColumnType t) noexcept {
    return t == ColumnType::Int64 || t == ColumnType::Uint64 ||
           t == ColumnType::Double;
}

bool is_text(ColumnType t) noexcept {
    return t == ColumnType::String || t == ColumnType::DictString;
}

// The types a dynamic column moves between; any other type is never promoted.
bool is_promotable(ColumnType t) noexcept {
    return is_number(t) || is_text(t) || t == ColumnType::Bool;
}

// The type that holds both `a`, a column's type, and `b`, a new value's type.
ColumnType merged_type(ColumnType a, ColumnType b) noexcept {
    if (a == b || !is_promotable(a) || !is_promotable(b)) return a;
    if (is_number(a) && is_number(b)) return ColumnType::Double;
    if (is_text(a) && is_text(b)) return a;
    return ColumnType::String;
}

dstd::String double_json_text(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
    dstd::String text = dstd::double_text(v);
    if (text.find_first_of(".eE") == dstd::String::npos) text += ".0";
    return text;
}

}  // namespace

struct RecordBatchBuilder::Scalar {
    enum class Kind : std::uint8_t { Int64, Uint64, Double, Bool, String };
    Kind kind = Kind::Int64;
    std::int64_t i = 0;
    std::uint64_t u = 0;
    double d = 0.0;
    bool b = false;
    dstd::StringView s;

    static Scalar of(std::int64_t v) {
        Scalar out;
        out.kind = Kind::Int64;
        out.i = v;
        return out;
    }
    static Scalar of(std::uint64_t v) {
        Scalar out;
        out.kind = Kind::Uint64;
        out.u = v;
        return out;
    }
    static Scalar of(double v) {
        Scalar out;
        out.kind = Kind::Double;
        out.d = v;
        return out;
    }
    static Scalar of(bool v) {
        Scalar out;
        out.kind = Kind::Bool;
        out.b = v;
        return out;
    }
    static Scalar of(dstd::StringView v) {
        Scalar out;
        out.kind = Kind::String;
        out.s = v;
        return out;
    }

    dstd::String text() const {
        switch (kind) {
            case Kind::Int64:
                return dstd::to_string(i);
            case Kind::Uint64:
                return dstd::to_string(u);
            case Kind::Double:
                return double_json_text(d);
            case Kind::Bool:
                return b ? "true" : "false";
            case Kind::String:
                break;
        }
        return dstd::String(s);
    }
};

void RecordBatchBuilder::init_column(ColumnData& col, ColumnType type,
                                     dstd::StringView name) {
    col.name = dstd::String(name);
    col.type = type;
    col.kind = storage_kind(type);
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
        case StorageKind::Int64:
            col.int64_values.resize(col.count + n, 0);
            break;
        case StorageKind::Uint64:
            col.uint64_values.resize(col.count + n, 0);
            break;
        case StorageKind::Double:
            col.double_values.resize(col.count + n, 0.0);
            break;
        case StorageKind::String:
            col.string_offsets.resize(
                col.count + n,
                static_cast<std::int32_t>(col.string_data.size()));
            break;
        case StorageKind::Bool:
            col.bool_values.resize(col.count + n, 0);
            break;
        case StorageKind::Dict:
            col.dict_indices.resize(col.count + n, -1);  // -1 = null
            break;
        case StorageKind::Hist:
            // Null rows contribute no buckets: repeat the current end offset.
            col.hist_offsets.resize(
                col.count + n, static_cast<std::int32_t>(col.hist_bins.size()));
            break;
        case StorageKind::StringList:
            col.list_offsets.resize(
                col.count + n,
                static_cast<std::int32_t>(col.string_offsets.size()));
            break;
        case StorageKind::Int64List:
            col.list_offsets.resize(
                col.count + n,
                static_cast<std::int32_t>(col.int64_values.size()));
            break;
        case StorageKind::StructList: {
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
    declare_schema(dstd::Vector<ColumnSpec>(specs));
}

void RecordBatchBuilder::declare_schema(const dstd::Vector<ColumnSpec>& specs) {
    columns_.clear();
    name_to_index_.clear();
    columns_.reserve(specs.size());
    touched_.assign(specs.size(), false);

    for (const auto& spec : specs) {
        std::size_t idx = columns_.size();
        columns_.emplace_back();
        init_column(columns_.back(), spec.type, spec.name);
        if (spec.type == ColumnType::StructList) {
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

std::size_t RecordBatchBuilder::add_or_get_column(dstd::StringView name,
                                                  ColumnType type) {
    auto it = name_to_index_.find(name);
    if (it != name_to_index_.end()) {
        ColumnData& col = columns_[it->second];
        if (col.type != type && !schema_declared_) [[unlikely]] {
            const ColumnType merged = merged_type(col.type, type);
            if (merged != col.type) promote_column(col, merged);
        }
        return it->second;
    }

    std::size_t idx = columns_.size();
    columns_.emplace_back();
    init_column(columns_.back(), type, name);
    if (num_rows_ > 0) {
        backfill_nulls(columns_.back(), num_rows_);
    }
    name_to_index_.emplace(dstd::String(name), idx);
    touched_.push_back(false);
    return idx;
}

void RecordBatchBuilder::promote_column(ColumnData& col, ColumnType target) {
    const std::size_t rows = col.count;
    if (target == ColumnType::Double) {
        col.double_values.resize(rows);
        if (col.kind == StorageKind::Int64) {
            for (std::size_t r = 0; r < rows; ++r)
                col.double_values[r] = static_cast<double>(col.int64_values[r]);
            col.int64_values.clear();
        } else {
            for (std::size_t r = 0; r < rows; ++r)
                col.double_values[r] =
                    static_cast<double>(col.uint64_values[r]);
            col.uint64_values.clear();
        }
    } else {
        col.string_offsets.clear();
        col.string_data.clear();
        col.string_offsets.reserve(rows);
        for (std::size_t r = 0; r < rows; ++r) {
            const bool is_null =
                (col.has_nulls && col.validity[r] == 0) ||
                (col.kind == StorageKind::Dict && col.dict_indices[r] < 0);
            if (!is_null) {
                switch (col.kind) {
                    case StorageKind::Int64: {
                        const dstd::String t =
                            dstd::to_string(col.int64_values[r]);
                        col.string_data.insert(col.string_data.end(), t.begin(),
                                               t.end());
                        break;
                    }
                    case StorageKind::Uint64: {
                        const dstd::String t =
                            dstd::to_string(col.uint64_values[r]);
                        col.string_data.insert(col.string_data.end(), t.begin(),
                                               t.end());
                        break;
                    }
                    case StorageKind::Double: {
                        const dstd::String t =
                            double_json_text(col.double_values[r]);
                        col.string_data.insert(col.string_data.end(), t.begin(),
                                               t.end());
                        break;
                    }
                    case StorageKind::Bool: {
                        const dstd::StringView t =
                            col.bool_values[r] ? "true" : "false";
                        col.string_data.insert(col.string_data.end(), t.begin(),
                                               t.end());
                        break;
                    }
                    case StorageKind::Dict: {
                        const dstd::String& t =
                            col.dict_values[static_cast<std::size_t>(
                                col.dict_indices[r])];
                        col.string_data.insert(col.string_data.end(), t.begin(),
                                               t.end());
                        break;
                    }
                    default:
                        break;
                }
            }
            col.string_offsets.push_back(
                static_cast<std::int32_t>(col.string_data.size()));
        }
        col.int64_values.clear();
        col.uint64_values.clear();
        col.double_values.clear();
        col.bool_values.clear();
        col.dict_indices.clear();
        col.dict_values.clear();
        col.dict_map.clear();
        if (!col.has_nulls) col.validity.clear();
    }
    col.type = target;
    col.kind = storage_kind(target);
}

dstd::Optional<std::size_t> RecordBatchBuilder::find_column(
    dstd::StringView name) const {
    auto it = name_to_index_.find(name);
    if (it != name_to_index_.end()) return it->second;
    return dstd::nullopt;
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

namespace {

template <class Values, class T>
void push_value(ColumnData& col, Values& values, T value) {
    values.push_back(value);
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
}

void push_text(ColumnData& col, dstd::StringView value) {
    col.string_data.insert(col.string_data.end(), value.begin(), value.end());
    col.string_offsets.push_back(
        static_cast<std::int32_t>(col.string_data.size()));
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
}

void push_dict(ColumnData& col, dstd::StringView value) {
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
}

}  // namespace

void RecordBatchBuilder::append_scalar_slow(std::size_t col_idx,
                                            const Scalar& v) {
    using Kind = Scalar::Kind;
    ColumnData& col = columns_[col_idx];
    switch (col.kind) {
        case StorageKind::Int64:
            if (v.kind == Kind::Int64) {
                push_value(col, col.int64_values, v.i);
                mark_touched(col_idx);
                return;
            }
            if (v.kind == Kind::Uint64 &&
                v.u <= static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())) {
                push_value(col, col.int64_values,
                           static_cast<std::int64_t>(v.u));
                mark_touched(col_idx);
                return;
            }
            break;
        case StorageKind::Uint64:
            if (v.kind == Kind::Uint64) {
                push_value(col, col.uint64_values, v.u);
                mark_touched(col_idx);
                return;
            }
            if (v.kind == Kind::Int64 && v.i >= 0) {
                push_value(col, col.uint64_values,
                           static_cast<std::uint64_t>(v.i));
                mark_touched(col_idx);
                return;
            }
            break;
        case StorageKind::Double:
            if (v.kind == Kind::Double || v.kind == Kind::Int64 ||
                v.kind == Kind::Uint64) {
                push_value(col, col.double_values,
                           v.kind == Kind::Double  ? v.d
                           : v.kind == Kind::Int64 ? static_cast<double>(v.i)
                                                   : static_cast<double>(v.u));
                mark_touched(col_idx);
                return;
            }
            break;
        case StorageKind::Bool:
            if (v.kind == Kind::Bool) {
                push_value(col, col.bool_values,
                           static_cast<std::uint8_t>(v.b ? 1 : 0));
                mark_touched(col_idx);
                return;
            }
            break;
        case StorageKind::String:
            if (col.type == ColumnType::String) {
                if (v.kind == Kind::String)
                    push_text(col, v.s);
                else
                    push_text(col, v.text());
                mark_touched(col_idx);
                return;
            }
            break;
        case StorageKind::Dict:
            if (v.kind == Kind::String)
                push_dict(col, v.s);
            else
                push_dict(col, v.text());
            mark_touched(col_idx);
            return;
        default:
            break;
    }
    append_null(col_idx);
}

void RecordBatchBuilder::append_int64(std::size_t col_idx, std::int64_t value) {
    auto& col = columns_[col_idx];
    if (col.kind != StorageKind::Int64) [[unlikely]] {
        append_scalar_slow(col_idx, Scalar::of(value));
        return;
    }
    push_value(col, col.int64_values, value);
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_uint64(std::size_t col_idx,
                                       std::uint64_t value) {
    auto& col = columns_[col_idx];
    if (col.kind != StorageKind::Uint64) [[unlikely]] {
        append_scalar_slow(col_idx, Scalar::of(value));
        return;
    }
    push_value(col, col.uint64_values, value);
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_double(std::size_t col_idx, double value) {
    auto& col = columns_[col_idx];
    if (col.kind != StorageKind::Double) [[unlikely]] {
        append_scalar_slow(col_idx, Scalar::of(value));
        return;
    }
    push_value(col, col.double_values, value);
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_string(std::size_t col_idx,
                                       dstd::StringView value) {
    auto& col = columns_[col_idx];
    if (col.kind != StorageKind::String) [[unlikely]] {
        append_scalar_slow(col_idx, Scalar::of(value));
        return;
    }
    push_text(col, value);
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_binary(std::size_t col_idx,
                                       dstd::StringView value) {
    // BINARY shares STRING's storage and append_string does no UTF-8
    // validation, so the two paths are identical.
    append_string(col_idx, value);
}

void RecordBatchBuilder::append_dict_string(std::size_t col_idx,
                                            dstd::StringView value) {
    auto& col = columns_[col_idx];
    if (col.kind != StorageKind::Dict) [[unlikely]] {
        append_scalar_slow(col_idx, Scalar::of(value));
        return;
    }
    push_dict(col, value);
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_bool(std::size_t col_idx, bool value) {
    auto& col = columns_[col_idx];
    if (col.kind != StorageKind::Bool) [[unlikely]] {
        append_scalar_slow(col_idx, Scalar::of(value));
        return;
    }
    push_value(col, col.bool_values, static_cast<std::uint8_t>(value ? 1 : 0));
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_hist(
    std::size_t col_idx, const dstd::Vector<dataframe::HistogramBin>& bins) {
    auto& col = columns_[col_idx];
    col.hist_bins.insert(col.hist_bins.end(), bins.begin(), bins.end());
    col.hist_offsets.push_back(static_cast<std::int32_t>(col.hist_bins.size()));
    if (col.has_nulls) col.validity.push_back(1);
    ++col.count;
    mark_touched(col_idx);
}

void RecordBatchBuilder::append_string_list(
    std::size_t col_idx, const dstd::Vector<dstd::StringView>& values) {
    auto& col = columns_[col_idx];
    for (dstd::StringView v : values) {
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
    std::size_t col_idx, const dstd::Vector<std::int64_t>& values) {
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
    std::size_t col_idx, const dstd::Vector<dstd::Vector<StructCell>>& rows) {
    auto& col = columns_[col_idx];
    for (const auto& inner : rows) {
        for (std::size_t f = 0; f < col.struct_children.size(); ++f) {
            ColumnData& fc = col.struct_children[f];
            const StructCell& cell = inner[f];
            switch (storage_kind(fc.type)) {
                case StorageKind::Double:
                    fc.double_values.push_back(cell.f64);
                    break;
                case StorageKind::Uint64:
                    fc.uint64_values.push_back(cell.u64);
                    break;
                case StorageKind::String:
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
        case StorageKind::Int64:
            col.int64_values.push_back(0);
            break;
        case StorageKind::Uint64:
            col.uint64_values.push_back(0);
            break;
        case StorageKind::Double:
            col.double_values.push_back(0.0);
            break;
        case StorageKind::String:
            col.string_offsets.push_back(
                static_cast<std::int32_t>(col.string_data.size()));
            break;
        case StorageKind::Bool:
            col.bool_values.push_back(0);
            break;
        case StorageKind::Dict:
            col.dict_indices.push_back(-1);  // -1 = null
            break;
        case StorageKind::Hist:
            col.hist_offsets.push_back(
                static_cast<std::int32_t>(col.hist_bins.size()));
            break;
        case StorageKind::StringList:
            col.list_offsets.push_back(
                static_cast<std::int32_t>(col.string_offsets.size()));
            break;
        case StorageKind::Int64List:
            col.list_offsets.push_back(
                static_cast<std::int32_t>(col.int64_values.size()));
            break;
        case StorageKind::StructList:
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
            case StorageKind::Int64:
                col.int64_values.reserve(num_rows);
                break;
            case StorageKind::Uint64:
                col.uint64_values.reserve(num_rows);
                break;
            case StorageKind::Double:
                col.double_values.reserve(num_rows);
                break;
            case StorageKind::String:
                col.string_offsets.reserve(num_rows + 1);
                // dftracer hash strings are 16 bytes; common strings
                // (event names, categories) range 4-32. Bumping the
                // initial reservation cuts geometric-growth memmove churn
                // visible in perf for moderate batch sizes.
                col.string_data.reserve(num_rows * 32);
                break;
            case StorageKind::Bool:
                col.bool_values.reserve(num_rows);
                break;
            case StorageKind::Dict:
                col.dict_indices.reserve(num_rows);
                break;
            case StorageKind::Hist:
                col.hist_offsets.reserve(num_rows);
                break;
            case StorageKind::StringList:
                col.list_offsets.reserve(num_rows);
                col.string_offsets.reserve(num_rows);
                col.string_data.reserve(num_rows * 16);
                break;
            case StorageKind::Int64List:
                col.list_offsets.reserve(num_rows);
                col.int64_values.reserve(num_rows);
                break;
            case StorageKind::StructList:
                col.list_offsets.reserve(num_rows);
                break;
        }
        col.validity.reserve(num_rows);
    }
}

dstd::Result<ArrowExportResult> RecordBatchBuilder::finish() {
    const std::int64_t ncols = static_cast<std::int64_t>(columns_.size());
    const std::int64_t nrows = static_cast<std::int64_t>(num_rows_);

    nanoarrow::UniqueSchema schema;
    if (ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_STRUCT) !=
        NANOARROW_OK) {
        return common::make_error(common::ErrorCode::Internal,
                                  "ArrowSchemaInitFromType(STRUCT) failed");
    }
    if (ArrowSchemaAllocateChildren(schema.get(), ncols) != NANOARROW_OK) {
        return common::make_error(common::ErrorCode::Internal,
                                  "ArrowSchemaAllocateChildren failed");
    }
    for (std::int64_t i = 0; i < ncols; ++i) {
        const auto& col = columns_[static_cast<std::size_t>(i)];
        ArrowSchema* child_schema = schema->children[i];

        if (col.type == ColumnType::DictString) {
            // Dictionary-encoded string: indices are INT32, values are STRING
            if (ArrowSchemaInitFromType(child_schema, NANOARROW_TYPE_INT32) !=
                NANOARROW_OK) {
                return common::make_error(
                    common::ErrorCode::Internal,
                    "ArrowSchemaInitFromType(dict indices) failed");
            }
            if (ArrowSchemaAllocateDictionary(child_schema) != NANOARROW_OK) {
                return common::make_error(
                    common::ErrorCode::Internal,
                    "ArrowSchemaAllocateDictionary failed");
            }
            if (ArrowSchemaInitFromType(child_schema->dictionary,
                                        NANOARROW_TYPE_STRING) !=
                NANOARROW_OK) {
                return common::make_error(
                    common::ErrorCode::Internal,
                    "ArrowSchemaInitFromType(dict values) failed");
            }
        } else if (col.type == ColumnType::Hist) {
            // list<struct<lo:double, hi:double, count:uint64>>
            if (ArrowSchemaInitFromType(child_schema, NANOARROW_TYPE_LIST) !=
                NANOARROW_OK) {
                return common::make_error(
                    common::ErrorCode::Internal,
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
                return common::make_error(common::ErrorCode::Internal,
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
                    return common::make_error(common::ErrorCode::Internal,
                                              "hist field schema init failed");
                }
            }
        } else if (col.type == ColumnType::StringList) {
            // Set item type in place; see the HIST note above.
            if (ArrowSchemaInitFromType(child_schema, NANOARROW_TYPE_LIST) !=
                NANOARROW_OK) {
                return common::make_error(
                    common::ErrorCode::Internal,
                    "ArrowSchemaInitFromType(list) failed");
            }
            if (ArrowSchemaSetType(child_schema->children[0],
                                   NANOARROW_TYPE_STRING) != NANOARROW_OK) {
                return common::make_error(
                    common::ErrorCode::Internal,
                    "string-list item schema init failed");
            }
        } else if (col.type == ColumnType::Int64List) {
            // Set item type in place; see the HIST note above.
            if (ArrowSchemaInitFromType(child_schema, NANOARROW_TYPE_LIST) !=
                NANOARROW_OK) {
                return common::make_error(
                    common::ErrorCode::Internal,
                    "ArrowSchemaInitFromType(list) failed");
            }
            if (ArrowSchemaSetType(child_schema->children[0],
                                   NANOARROW_TYPE_INT64) != NANOARROW_OK) {
                return common::make_error(common::ErrorCode::Internal,
                                          "int64-list item schema init failed");
            }
        } else if (col.type == ColumnType::StructList) {
            // Set item type in place; see the HIST note above.
            if (ArrowSchemaInitFromType(child_schema, NANOARROW_TYPE_LIST) !=
                NANOARROW_OK) {
                return common::make_error(
                    common::ErrorCode::Internal,
                    "ArrowSchemaInitFromType(list) failed");
            }
            ArrowSchema* item = child_schema->children[0];
            const std::int64_t nfields =
                static_cast<std::int64_t>(col.struct_fields.size());
            if (ArrowSchemaSetType(item, NANOARROW_TYPE_STRUCT) !=
                    NANOARROW_OK ||
                ArrowSchemaAllocateChildren(item, nfields) != NANOARROW_OK) {
                return common::make_error(common::ErrorCode::Internal,
                                          "struct-list schema init failed");
            }
            for (std::int64_t f = 0; f < nfields; ++f) {
                const auto& fs = col.struct_fields[static_cast<std::size_t>(f)];
                if (ArrowSchemaInitFromType(item->children[f],
                                            to_nanoarrow_type(fs.type)) !=
                        NANOARROW_OK ||
                    ArrowSchemaSetName(item->children[f], fs.name.c_str()) !=
                        NANOARROW_OK) {
                    return common::make_error(
                        common::ErrorCode::Internal,
                        "struct-list field schema init failed");
                }
            }
        } else {
            if (ArrowSchemaInitFromType(child_schema,
                                        to_nanoarrow_type(col.type)) !=
                NANOARROW_OK) {
                return common::make_error(
                    common::ErrorCode::Internal,
                    "ArrowSchemaInitFromType(child) failed");
            }
        }
        if (ArrowSchemaSetName(child_schema, col.name.c_str()) !=
            NANOARROW_OK) {
            return common::make_error(common::ErrorCode::Internal,
                                      "ArrowSchemaSetName failed");
        }
    }

    nanoarrow::UniqueArray array;
    if (ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr) !=
        NANOARROW_OK) {
        return common::make_error(common::ErrorCode::Internal,
                                  "ArrowArrayInitFromSchema failed");
    }
    // StartAppending initialises children recursively.
    if (ArrowArrayStartAppending(array.get()) != NANOARROW_OK) {
        return common::make_error(common::ErrorCode::Internal,
                                  "ArrowArrayStartAppending failed");
    }

    for (std::int64_t i = 0; i < ncols; ++i) {
        const auto& col = columns_[static_cast<std::size_t>(i)];
        ArrowArray* child = array->children[i];

        if (ArrowArrayReserve(child, nrows) != NANOARROW_OK) {
            return common::make_error(common::ErrorCode::Internal,
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
            case ColumnType::Int64: {
                fill_validity();
                ArrowBuffer* data_buf = ArrowArrayBuffer(child, 1);
                if (ArrowBufferAppend(data_buf, col.int64_values.data(),
                                      static_cast<std::int64_t>(
                                          row_count * sizeof(std::int64_t))) !=
                    NANOARROW_OK) {
                    return common::make_error(common::ErrorCode::Internal,
                                              "INT64 buffer append failed");
                }
                break;
            }
            case ColumnType::Uint64: {
                fill_validity();
                ArrowBuffer* data_buf = ArrowArrayBuffer(child, 1);
                if (ArrowBufferAppend(data_buf, col.uint64_values.data(),
                                      static_cast<std::int64_t>(
                                          row_count * sizeof(std::uint64_t))) !=
                    NANOARROW_OK) {
                    return common::make_error(common::ErrorCode::Internal,
                                              "UINT64 buffer append failed");
                }
                break;
            }
            case ColumnType::Double: {
                fill_validity();
                ArrowBuffer* data_buf = ArrowArrayBuffer(child, 1);
                if (ArrowBufferAppend(data_buf, col.double_values.data(),
                                      static_cast<std::int64_t>(
                                          row_count * sizeof(double))) !=
                    NANOARROW_OK) {
                    return common::make_error(common::ErrorCode::Internal,
                                              "DOUBLE buffer append failed");
                }
                break;
            }
            case ColumnType::String:
            case ColumnType::Binary: {
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
            case ColumnType::Bool: {
                for (std::size_t r = 0; r < row_count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            return common::make_error(
                                common::ErrorCode::Internal,
                                "ArrowArrayAppendNull(bool) failed");
                        }
                        ++null_count;
                    } else {
                        if (ArrowArrayAppendInt(child, col.bool_values[r]) !=
                            NANOARROW_OK) {
                            return common::make_error(
                                common::ErrorCode::Internal,
                                "ArrowArrayAppendInt(bool) failed");
                        }
                    }
                }
                break;
            }
            case ColumnType::DictString: {
                for (std::size_t r = 0; r < col.count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            return common::make_error(
                                common::ErrorCode::Internal,
                                "ArrowArrayAppendNull(dict) failed");
                        }
                    } else {
                        if (ArrowArrayAppendInt(child, col.dict_indices[r]) !=
                            NANOARROW_OK) {
                            return common::make_error(
                                common::ErrorCode::Internal,
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
                    return common::make_error(common::ErrorCode::Internal,
                                              "Failed to allocate dictionary");
                }
                ArrowArrayInitFromType(child->dictionary,
                                       NANOARROW_TYPE_STRING);
                if (ArrowArrayStartAppending(child->dictionary) !=
                    NANOARROW_OK) {
                    return common::make_error(
                        common::ErrorCode::Internal,
                        "ArrowArrayStartAppending(dict) failed");
                }
                if (ArrowArrayReserve(
                        child->dictionary,
                        static_cast<std::int64_t>(col.dict_values.size())) !=
                    NANOARROW_OK) {
                    return common::make_error(common::ErrorCode::Internal,
                                              "ArrowArrayReserve(dict) failed");
                }
                for (const auto& s : col.dict_values) {
                    ArrowStringView asv{s.data(),
                                        static_cast<std::int64_t>(s.size())};
                    if (ArrowArrayAppendString(child->dictionary, asv) !=
                        NANOARROW_OK) {
                        return common::make_error(
                            common::ErrorCode::Internal,
                            "ArrowArrayAppendString(dict) failed");
                    }
                }
                if (ArrowArrayFinishBuildingDefault(child->dictionary,
                                                    nullptr) != NANOARROW_OK) {
                    return common::make_error(
                        common::ErrorCode::Internal,
                        "ArrowArrayFinishBuildingDefault(dict) failed");
                }
                break;
            }
            case ColumnType::Hist: {
                ArrowArray* st = child->children[0];
                ArrowArray* c_lo = st->children[0];
                ArrowArray* c_hi = st->children[1];
                ArrowArray* c_cnt = st->children[2];
                std::size_t pos = 0;
                for (std::size_t r = 0; r < row_count; ++r) {
                    const std::int32_t end = col.hist_offsets[r];
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            return common::make_error(
                                common::ErrorCode::Internal,
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
                            return common::make_error(
                                common::ErrorCode::Internal,
                                "hist bucket append failed");
                        }
                    }
                    if (ArrowArrayFinishElement(child) != NANOARROW_OK) {
                        return common::make_error(
                            common::ErrorCode::Internal,
                            "ArrowArrayFinishElement(hist) failed");
                    }
                }
                break;
            }
            case ColumnType::StringList: {
                ArrowArray* item = child->children[0];
                std::size_t pos = 0;
                for (std::size_t r = 0; r < row_count; ++r) {
                    const std::int32_t end = col.list_offsets[r];
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            return common::make_error(
                                common::ErrorCode::Internal,
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
                            return common::make_error(
                                common::ErrorCode::Internal,
                                "ArrowArrayAppendString(string-list) failed");
                        }
                    }
                    if (ArrowArrayFinishElement(child) != NANOARROW_OK) {
                        return common::make_error(
                            common::ErrorCode::Internal,
                            "ArrowArrayFinishElement(string-list) failed");
                    }
                }
                break;
            }
            case ColumnType::Int64List: {
                ArrowArray* item = child->children[0];
                std::size_t pos = 0;
                for (std::size_t r = 0; r < row_count; ++r) {
                    const std::int32_t end = col.list_offsets[r];
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            return common::make_error(
                                common::ErrorCode::Internal,
                                "ArrowArrayAppendNull(int64-list) failed");
                        }
                        ++null_count;
                        pos = static_cast<std::size_t>(end);
                        continue;
                    }
                    for (; pos < static_cast<std::size_t>(end); ++pos) {
                        if (ArrowArrayAppendInt(item, col.int64_values[pos]) !=
                            NANOARROW_OK) {
                            return common::make_error(
                                common::ErrorCode::Internal,
                                "ArrowArrayAppendInt(int64-list) failed");
                        }
                    }
                    if (ArrowArrayFinishElement(child) != NANOARROW_OK) {
                        return common::make_error(
                            common::ErrorCode::Internal,
                            "ArrowArrayFinishElement(int64-list) failed");
                    }
                }
                break;
            }
            case ColumnType::StructList: {
                ArrowArray* st = child->children[0];
                const std::size_t nfields = col.struct_children.size();
                std::size_t pos = 0;
                for (std::size_t r = 0; r < row_count; ++r) {
                    const std::int32_t end = col.list_offsets[r];
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            return common::make_error(
                                common::ErrorCode::Internal,
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
                                case ColumnType::Double:
                                case ColumnType::Float32:
                                    ok = ArrowArrayAppendDouble(
                                             cf, fc.double_values[pos]) ==
                                         NANOARROW_OK;
                                    break;
                                case ColumnType::Uint64:
                                case ColumnType::Uint8:
                                case ColumnType::Uint16:
                                case ColumnType::Uint32:
                                    ok = ArrowArrayAppendUInt(
                                             cf, fc.uint64_values[pos]) ==
                                         NANOARROW_OK;
                                    break;
                                case ColumnType::String: {
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
                                case ColumnType::Binary: {
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
                                return common::make_error(
                                    common::ErrorCode::Internal,
                                    "struct-list field append failed");
                            }
                        }
                        if (ArrowArrayFinishElement(st) != NANOARROW_OK) {
                            return common::make_error(
                                common::ErrorCode::Internal,
                                "ArrowArrayFinishElement(struct) failed");
                        }
                    }
                    if (ArrowArrayFinishElement(child) != NANOARROW_OK) {
                        return common::make_error(
                            common::ErrorCode::Internal,
                            "ArrowArrayFinishElement(struct-list) failed");
                    }
                }
                break;
            }
            // Narrow widths append element-wise so nanoarrow writes the exact
            // Arrow width; the bulk path above assumes 8-byte cells.
            case ColumnType::Int8:
            case ColumnType::Int16:
            case ColumnType::Int32: {
                for (std::size_t r = 0; r < row_count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            return common::make_error(
                                common::ErrorCode::Internal,
                                "ArrowArrayAppendNull(int) failed");
                        }
                    } else if (ArrowArrayAppendInt(child,
                                                   col.int64_values[r]) !=
                               NANOARROW_OK) {
                        return common::make_error(common::ErrorCode::Internal,
                                                  "ArrowArrayAppendInt failed");
                    }
                }
                break;
            }
            case ColumnType::Uint8:
            case ColumnType::Uint16:
            case ColumnType::Uint32: {
                for (std::size_t r = 0; r < row_count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            return common::make_error(
                                common::ErrorCode::Internal,
                                "ArrowArrayAppendNull(uint) failed");
                        }
                    } else if (ArrowArrayAppendUInt(child,
                                                    col.uint64_values[r]) !=
                               NANOARROW_OK) {
                        return common::make_error(
                            common::ErrorCode::Internal,
                            "ArrowArrayAppendUInt failed");
                    }
                }
                break;
            }
            case ColumnType::Float32: {
                for (std::size_t r = 0; r < row_count; ++r) {
                    if (col.has_nulls && col.validity[r] == 0) {
                        if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
                            return common::make_error(
                                common::ErrorCode::Internal,
                                "ArrowArrayAppendNull(float32) failed");
                        }
                    } else if (ArrowArrayAppendDouble(child,
                                                      col.double_values[r]) !=
                               NANOARROW_OK) {
                        return common::make_error(
                            common::ErrorCode::Internal,
                            "ArrowArrayAppendDouble(float32) failed");
                    }
                }
                break;
            }
        }

        if (col.type == ColumnType::Int64 || col.type == ColumnType::Uint64 ||
            col.type == ColumnType::Double || col.type == ColumnType::String ||
            col.type == ColumnType::Binary) {
            child->length = static_cast<std::int64_t>(row_count);
            child->null_count = col.has_nulls ? null_count : 0;
        }

        if (ArrowArrayFinishBuildingDefault(child, nullptr) != NANOARROW_OK) {
            return common::make_error(
                common::ErrorCode::Internal,
                "ArrowArrayFinishBuildingDefault(child) failed");
        }
    }

    array->length = nrows;
    array->null_count = 0;

    if (ArrowArrayFinishBuildingDefault(array.get(), nullptr) != NANOARROW_OK) {
        return common::make_error(
            common::ErrorCode::Internal,
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

}  // namespace dftracer::utils::utilities::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
