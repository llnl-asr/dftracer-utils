// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_DATAFRAME_BUILDERS_TYPES_H
#define DFTRACER_UTILS_TESTS_DATAFRAME_BUILDERS_TYPES_H

// Native builders for the logical types that have no public Series
// constructor: temporal units, decimals, fixed-size binary, wide-offset text,
// large/fixed-size lists and maps. Each one writes the column layout directly
// (the same one the Arrow import produces), so a test builds its input without
// going through Arrow. Header-only so a doctest TU can use REQUIRE inside them.

#include <dftracer/utils/dataframe/internal/buffers.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/float16.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <utility>

namespace dftracer::utils::dataframe::test_types {

using Cells = dstd::Vector<dstd::Optional<dstd::String>>;

/// A FLAT column of `type` with `n` rows, every buffer still empty.
inline dftu_series* new_column(TypeId type, std::int64_t n) {
    auto* col = new dftu_series();
    col->type = type;
    col->encoding = Encoding::Flat;
    col->length = n;
    return col;
}

/// Copy `validity` (Arrow layout, 1 = valid) into `col` and set its null count.
inline void set_validity(dftu_series* col, const std::uint8_t* validity,
                         std::int64_t n) {
    if (validity == nullptr || n <= 0) return;
    const std::size_t bytes = static_cast<std::size_t>((n + 7) / 8);
    col->validity = new_buffer(bytes);
    std::memcpy(writable(col->validity), validity, bytes);
    for (std::int64_t i = 0; i < n; ++i)
        col->null_count += !((validity[i >> 3] >> (i & 7)) & 1);
}

/// The validity bitmap of `values`: null where the cell is empty.
inline dstd::Vector<std::uint8_t> cells_validity(const Cells& values) {
    dstd::Vector<std::uint8_t> validity;
    bool any_null = false;
    for (const auto& v : values)
        if (!v) any_null = true;
    if (!any_null) return validity;
    validity.assign((values.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < values.size(); ++i)
        if (values[i])
            validity[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    return validity;
}

/// A FLAT fixed-width column holding `values`.
template <class T>
Series make_fixed(TypeId type, const dstd::Vector<T>& values,
                  const std::uint8_t* validity = nullptr) {
    const std::int64_t n = static_cast<std::int64_t>(values.size());
    auto* col = new_column(type, n);
    if (n > 0) {
        const auto bytes = static_cast<std::size_t>(n) * sizeof(T);
        col->data = new_buffer(bytes);
        std::memcpy(writable(col->data), values.data(), bytes);
    }
    set_validity(col, validity, n);
    return Series{col};
}

/// A FLAT variable-width column: int32 offsets for String/Binary, int64 for
/// LargeString/LargeBinary.
inline Series make_varwidth(TypeId type, const Cells& values) {
    const std::int64_t n = static_cast<std::int64_t>(values.size());
    const bool wide =
        type == TypeId::LargeString || type == TypeId::LargeBinary;
    auto* col = new_column(type, n);
    dstd::String bytes;
    for (const auto& v : values)
        if (v) bytes += *v;
    const std::size_t off_size =
        wide ? sizeof(std::int64_t) : sizeof(std::int32_t);
    col->offsets = new_buffer(static_cast<std::size_t>(n + 1) * off_size);
    auto* off = writable(col->offsets);
    std::int64_t at = 0;
    for (std::int64_t i = 0; i <= n; ++i) {
        if (i > 0 && values[static_cast<std::size_t>(i - 1)])
            at += static_cast<std::int64_t>(
                values[static_cast<std::size_t>(i - 1)]->size());
        if (wide) {
            reinterpret_cast<std::int64_t*>(off)[i] = at;
        } else {
            reinterpret_cast<std::int32_t*>(off)[i] =
                static_cast<std::int32_t>(at);
        }
    }
    if (!bytes.empty()) {
        col->data = new_buffer(bytes.size());
        std::memcpy(writable(col->data), bytes.data(), bytes.size());
    }
    const dstd::Vector<std::uint8_t> validity = cells_validity(values);
    set_validity(col, validity.empty() ? nullptr : validity.data(), n);
    return Series{col};
}

inline Series make_string(const Cells& values) {
    return make_varwidth(TypeId::String, values);
}

inline Series make_large_utf8(const Cells& values) {
    return make_varwidth(TypeId::LargeString, values);
}

inline Series make_binary(const Cells& values) {
    return make_varwidth(TypeId::Binary, values);
}

inline Series make_large_binary(const Cells& values) {
    return make_varwidth(TypeId::LargeBinary, values);
}

/// Float16 from float inputs, rounded the same way an Arrow append does.
inline Series make_float16(const dstd::Vector<float>& values) {
    dstd::Vector<std::uint16_t> bits;
    bits.reserve(values.size());
    for (float f : values) bits.push_back(dataframe::float_to_half(f));
    return make_fixed(TypeId::Float16, bits);
}

/// A decimal column of `type` holding the unscaled integers `values` as
/// little-endian two's complement, `precision`/`scale` as declared.
inline Series make_decimal(TypeId type,
                           const dstd::Vector<std::int64_t>& unscaled,
                           std::int32_t precision, std::int32_t scale) {
    const std::size_t width = type == TypeId::Decimal256 ? 32 : 16;
    const std::int64_t n = static_cast<std::int64_t>(unscaled.size());
    auto* col = new_column(type, n);
    col->set_decimal(precision, scale);
    if (n > 0) {
        col->data = new_buffer(static_cast<std::size_t>(n) * width);
        auto* out = writable(col->data);
        for (std::int64_t i = 0; i < n; ++i) {
            auto* row = out + static_cast<std::size_t>(i) * width;
            std::memset(row,
                        unscaled[static_cast<std::size_t>(i)] < 0 ? 0xFF : 0,
                        width);
            std::memcpy(row, &unscaled[static_cast<std::size_t>(i)],
                        sizeof(std::int64_t));
        }
    }
    return Series{col};
}

inline Series make_decimal128(const dstd::Vector<std::int64_t>& values,
                              std::int32_t precision = 38,
                              std::int32_t scale = 0) {
    return make_decimal(TypeId::Decimal128, values, precision, scale);
}

inline Series make_decimal256(const dstd::Vector<std::int64_t>& values,
                              std::int32_t precision = 38,
                              std::int32_t scale = 0) {
    return make_decimal(TypeId::Decimal256, values, precision, scale);
}

inline Series make_fixed_size_binary(const dstd::Vector<dstd::String>& values,
                                     std::int32_t width) {
    const std::int64_t n = static_cast<std::int64_t>(values.size());
    auto* col = new_column(TypeId::FixedSizeBinary, n);
    col->set_fixed_size(width);
    if (n > 0) {
        const auto bytes =
            static_cast<std::size_t>(n) * static_cast<std::size_t>(width);
        col->data = new_buffer(bytes);
        auto* out = writable(col->data);
        for (std::int64_t i = 0; i < n; ++i) {
            const dstd::String& v = values[static_cast<std::size_t>(i)];
            REQUIRE(static_cast<std::int32_t>(v.size()) == width);
            std::memcpy(out + static_cast<std::size_t>(i) *
                                  static_cast<std::size_t>(width),
                        v.data(), static_cast<std::size_t>(width));
        }
    }
    return Series{col};
}

/// An Int64 child holding `rows` flattened; returns the element count.
inline std::int64_t fill_children(
    const dstd::Vector<dstd::Vector<std::int64_t>>& rows,
    dstd::Vector<std::int64_t>& flat) {
    std::int64_t total = 0;
    for (const auto& row : rows) total += static_cast<std::int64_t>(row.size());
    flat.reserve(static_cast<std::size_t>(total));
    for (const auto& row : rows)
        for (std::int64_t v : row) flat.push_back(v);
    return total;
}

inline Series make_large_list_i64(
    const dstd::Vector<dstd::Vector<std::int64_t>>& rows) {
    const std::int64_t n = static_cast<std::int64_t>(rows.size());
    auto* col = new_column(TypeId::LargeList, n);
    dstd::Vector<std::int64_t> flat;
    fill_children(rows, flat);
    col->offsets =
        new_buffer(static_cast<std::size_t>(n + 1) * sizeof(std::int64_t));
    auto* off = reinterpret_cast<std::int64_t*>(writable(col->offsets));
    off[0] = 0;
    for (std::int64_t i = 0; i < n; ++i)
        off[i + 1] = off[i] + static_cast<std::int64_t>(
                                  rows[static_cast<std::size_t>(i)].size());
    Series child = make_fixed(TypeId::Int64, flat);
    col->set_child(adopt_series(child.release()));
    return Series{col};
}

/// A LargeList column whose `offsets` (n+1 entries) index into `child`.
inline Series make_large_list(const dstd::Vector<std::int64_t>& offsets,
                              Series child) {
    auto* col = new_column(TypeId::LargeList,
                           static_cast<std::int64_t>(offsets.size()) - 1);
    col->offsets = new_buffer(offsets.size() * sizeof(std::int64_t));
    std::memcpy(writable(col->offsets), offsets.data(),
                offsets.size() * sizeof(std::int64_t));
    col->set_child(adopt_series(child.release()));
    return Series{col};
}

/// Every row must hold exactly `rows[0].size()` elements; that count becomes
/// the FixedSizeList width.
inline Series make_fixed_size_list_i64(
    const dstd::Vector<dstd::Vector<std::int64_t>>& rows) {
    REQUIRE_FALSE(rows.empty());
    const std::int32_t width = static_cast<std::int32_t>(rows[0].size());
    const std::int64_t n = static_cast<std::int64_t>(rows.size());
    auto* col = new_column(TypeId::FixedSizeList, n);
    col->set_fixed_size(width);
    dstd::Vector<std::int64_t> flat;
    for (const auto& row : rows)
        REQUIRE(static_cast<std::int32_t>(row.size()) == width);
    fill_children(rows, flat);
    Series child = make_fixed(TypeId::Int64, flat);
    col->set_child(adopt_series(child.release()));
    return Series{col};
}

/// A Map column: int32 offsets over a Struct{key: String, value: Int64} child.
inline Series make_map_string_i64(
    const dstd::Vector<dstd::Vector<dstd::Pair<dstd::String, std::int64_t>>>&
        rows) {
    const std::int64_t n = static_cast<std::int64_t>(rows.size());
    auto* col = new_column(TypeId::Map, n);
    col->offsets =
        new_buffer(static_cast<std::size_t>(n + 1) * sizeof(std::int32_t));
    auto* off = reinterpret_cast<std::int32_t*>(writable(col->offsets));
    Cells keys;
    dstd::Vector<std::int64_t> values;
    off[0] = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        for (const auto& kv : rows[static_cast<std::size_t>(i)]) {
            keys.push_back(kv.first);
            values.push_back(kv.second);
        }
        off[i + 1] = static_cast<std::int32_t>(keys.size());
    }
    dstd::Vector<Series> fields;
    fields.push_back(make_string(keys));
    fields.push_back(make_fixed(TypeId::Int64, values));
    Series entries = Series::structs({"key", "value"}, std::move(fields));
    col->set_child(adopt_series(entries.release()));
    return Series{col};
}

/// A temporal column: the declared unit (and, for Timestamp, `timezone`, null
/// for naive).
template <class T>
Series make_temporal(TypeId type, const dstd::Vector<T>& values,
                     TimeUnit unit) {
    Series s = make_fixed(type, values);
    s.handle()->set_time_unit(unit);
    return s;
}

inline Series make_timestamp(const dstd::Vector<std::int64_t>& values,
                             TimeUnit unit = TimeUnit::Micro,
                             const char* timezone = "UTC") {
    Series s = make_temporal(TypeId::Timestamp, values, unit);
    if (timezone != nullptr) s.handle()->set_timezone(timezone);
    return s;
}

inline Series make_time32(const dstd::Vector<std::int32_t>& values,
                          TimeUnit unit = TimeUnit::Milli) {
    return make_temporal(TypeId::Time32, values, unit);
}

inline Series make_time64(const dstd::Vector<std::int64_t>& values,
                          TimeUnit unit = TimeUnit::Micro) {
    return make_temporal(TypeId::Time64, values, unit);
}

inline Series make_duration(const dstd::Vector<std::int64_t>& values,
                            TimeUnit unit = TimeUnit::Micro) {
    return make_temporal(TypeId::Duration, values, unit);
}

inline Series make_date32(const dstd::Vector<std::int32_t>& values) {
    return make_fixed(TypeId::Date32, values);
}

inline Series make_date64(const dstd::Vector<std::int64_t>& values) {
    return make_fixed(TypeId::Date64, values);
}

}  // namespace dftracer::utils::dataframe::test_types

#endif  // DFTRACER_UTILS_TESTS_DATAFRAME_BUILDERS_TYPES_H
