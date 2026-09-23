#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_COLUMN_READ_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_COLUMN_READ_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/decimal.h>
#include <dftracer/utils/dataframe/internal/float16.h>

#include <cstdint>
#include <string_view>

// Read one FLAT numeric cell widened to a common type, dispatching on the
// column's TypeId. Shared by every kernel that needs a domain-agnostic value
// (stats, aggregation, hashing, ...) so the per-type switch lives in one place.
namespace dftracer::utils::dataframe {

/// Cell `i` widened to int64 (Bool -> 0/1). Dispatches on physical_type(),
/// so Date32/Time32 read as Int32 and Date64/Time64/Timestamp/Duration read
/// as Int64: their exact on-disk value, not a lossy conversion. 0 for a
/// column with no integer physical layout.
inline std::int64_t read_i64(const Series& c, std::int64_t i) {
    switch (physical_type(c.type())) {
        case TypeId::Bool: {
            const std::uint8_t* b = c.data<std::uint8_t>();
            return (b[i >> 3] >> (i & 7)) & 1;
        }
        case TypeId::Int8:
            return c.data<std::int8_t>()[i];
        case TypeId::Int16:
            return c.data<std::int16_t>()[i];
        case TypeId::Int32:
            return c.data<std::int32_t>()[i];
        case TypeId::Int64:
            return c.data<std::int64_t>()[i];
        default:
            return 0;
    }
}

/// Cell `i` widened to uint64: unsigned columns keep their value; signed/Bool
/// are read as int64 then reinterpreted (so hashing sees a stable bit pattern).
/// 0 for non-integer columns.
inline std::uint64_t read_u64(const Series& c, std::int64_t i) {
    switch (c.type()) {
        case TypeId::Uint8:
            return c.data<std::uint8_t>()[i];
        case TypeId::Uint16:
            return c.data<std::uint16_t>()[i];
        case TypeId::Uint32:
            return c.data<std::uint32_t>()[i];
        case TypeId::Uint64:
            return c.data<std::uint64_t>()[i];
        default:
            return static_cast<std::uint64_t>(read_i64(c, i));
    }
}

/// Cell `i` widened to double (Bool -> 0/1). Float16 decodes its half bits
/// exactly; Decimal128/256 decode the scaled integer at the documented Float64
/// precision loss, so use read_bytes when equality must be exact. 0 for a
/// column whose value_domain() is not Numeric.
inline double read_f64(const Series& c, std::int64_t i) {
    switch (c.type()) {
        case TypeId::Float16:
            return static_cast<double>(
                half_to_float(c.data<std::uint16_t>()[i]));
        case TypeId::Float32:
            return static_cast<double>(c.data<float>()[i]);
        case TypeId::Float64:
            return c.data<double>()[i];
        case TypeId::Decimal128:
            return decimal128_to_double(
                c.data<std::uint8_t>() + static_cast<std::size_t>(i) * 16,
                c.data_type().decimal_scale);
        case TypeId::Decimal256:
            return decimal256_to_double(
                c.data<std::uint8_t>() + static_cast<std::size_t>(i) * 32,
                c.data_type().decimal_scale);
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
            return static_cast<double>(read_u64(c, i));
        default:
            return static_cast<double>(read_i64(c, i));
    }
}

/// Byte view of cell `i` for a ValueDomain::Bytes column, valid while `c`
/// lives. Empty for any other column, and, like Series::string_at, for a
/// non-FLAT one.
inline std::string_view read_bytes(const Series& c, std::int64_t i) {
    const TypeId t = c.type();
    if (narrow_varwidth_type(t) == TypeId::String ||
        narrow_varwidth_type(t) == TypeId::Binary)
        return c.string_at(i);
    std::size_t width;
    if (t == TypeId::FixedSizeBinary)
        width = static_cast<std::size_t>(c.data_type().fixed_size);
    else if (t == TypeId::Decimal128 || t == TypeId::Decimal256)
        width = byte_width(t).value_or(0);
    else
        return {};
    const char* d = static_cast<const char*>(dftu_series_data(c.handle()));
    if (d == nullptr || width == 0) return {};
    return std::string_view(d + static_cast<std::size_t>(i) * width, width);
}

/// True when `t` is nested and so has no per-row value for `op` to work on,
/// after logging a named refusal. The caller then returns its own refusal
/// value instead of computing over a zero it never read.
inline bool refuse_nested_value(const char* op, TypeId t) {
    if (value_domain(t) != ValueDomain::None) return false;
    DFTRACER_UTILS_LOG_ERROR("%s: no per-row value for nested type '%s'", op,
                             type_name(t));
    return true;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_COLUMN_READ_H
