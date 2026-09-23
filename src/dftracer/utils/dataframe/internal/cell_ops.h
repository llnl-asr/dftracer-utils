#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_CELL_OPS_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_CELL_OPS_H

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Small per-cell helpers shared by the eager batch ops and the lazy cursors, so
// neither owns a private copy that could drift. These operate on one cell at a
// time (string building, type tests) and are inherently scalar; bulk numeric
// work stays in the SIMD kernels.
namespace dftracer::utils::dataframe {

/// Integer/float column (Bool and variable-width/nested types excluded). The
/// column set for describe and other numeric-only ops.
inline bool is_numeric(TypeId t) {
    switch (t) {
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
        case TypeId::Float32:
        case TypeId::Float64:
            return true;
        default:
            return false;
    }
}

/// Round `x` down to a multiple of `m` (m > 0), toward negative infinity. The
/// group_by_dynamic window grid.
inline std::int64_t floor_to_multiple(std::int64_t x, std::int64_t m) {
    std::int64_t q = x / m;
    if ((x % m) != 0 && x < 0) --q;
    return q * m;
}

/// String form of a cell, for pivot/to_dummies column naming. Every
/// value_domain() != None type renders a distinct label.
inline std::string cell_to_string(const Series& c, std::int64_t i) {
    switch (c.type()) {
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
        case TypeId::FixedSizeBinary:
            return std::string(read_bytes(c, i));
        case TypeId::Bool:
            return ((c.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1) ? "true"
                                                                     : "false";
        case TypeId::Int8:
            return std::to_string(c.data<std::int8_t>()[i]);
        case TypeId::Int16:
            return std::to_string(c.data<std::int16_t>()[i]);
        case TypeId::Int32:
            return std::to_string(c.data<std::int32_t>()[i]);
        case TypeId::Int64:
            return std::to_string(c.data<std::int64_t>()[i]);
        case TypeId::Uint8:
            return std::to_string(c.data<std::uint8_t>()[i]);
        case TypeId::Uint16:
            return std::to_string(c.data<std::uint16_t>()[i]);
        case TypeId::Uint32:
            return std::to_string(c.data<std::uint32_t>()[i]);
        case TypeId::Uint64:
            return std::to_string(c.data<std::uint64_t>()[i]);
        case TypeId::Float32:
            return std::to_string(c.data<float>()[i]);
        case TypeId::Float64:
        case TypeId::Float16:
        case TypeId::Decimal128:
        case TypeId::Decimal256:
            return std::to_string(read_f64(c, i));
        case TypeId::Date32:
        case TypeId::Date64:
        case TypeId::Time32:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
            return std::to_string(read_i64(c, i));
        default:
            return std::string();
    }
}

/// Append an exact byte encoding of cell `i` of `c` to `key`: a null flag then
/// the raw bytes (length-prefixed for byte-domain cells), so distinct cells
/// never collide and equal cells always match. The building block for row
/// dedupe keys. Throws std::invalid_argument for a nested column.
inline void append_cell(std::string& key, const Series& c, std::int64_t i) {
    if (!is_orderable_type(c.type()))
        throw std::invalid_argument(std::string("column type '") +
                                    type_name(c.type()) +
                                    "' has no per-row value to key on");
    if (c.is_null(i)) {
        key.push_back('\0');
        return;
    }
    key.push_back('\1');
    const TypeId t = c.type();
    if (value_domain(t) == ValueDomain::Bytes) {
        const std::string_view s = read_bytes(c, i);
        const auto len = static_cast<std::int32_t>(s.size());
        key.append(reinterpret_cast<const char*>(&len), sizeof(len));
        key.append(s.data(), s.size());
        return;
    }
    const auto* d =
        static_cast<const std::uint8_t*>(dftu_series_data(c.handle()));
    if (t == TypeId::Bool) {
        key.push_back((d[i >> 3] >> (i & 7)) & 1 ? '\1' : '\0');
        return;
    }
    const std::size_t w = byte_width(t).value_or(0);
    key.append(
        reinterpret_cast<const char*>(d + static_cast<std::size_t>(i) * w), w);
}

/// Exact dedupe key of row `i` across `cols` (already FLAT).
inline std::string row_key(const std::vector<Series>& cols, std::int64_t i) {
    std::string k;
    for (const Series& c : cols) append_cell(k, c, i);
    return k;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_CELL_OPS_H
