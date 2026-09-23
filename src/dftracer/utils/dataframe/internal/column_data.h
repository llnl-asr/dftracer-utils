#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_COLUMN_DATA_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_COLUMN_DATA_H

#include <dftracer/utils/dataframe/buffer.h>
#include <dftracer/utils/dataframe/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/// Layout behind the opaque dftu_series handle. Private to the dataframe
/// sources; the public headers only forward-declare it.
struct dftu_series {
    dftracer::utils::dataframe::TypeId type =
        dftracer::utils::dataframe::TypeId::Int64;
    dftracer::utils::dataframe::Encoding encoding =
        dftracer::utils::dataframe::Encoding::Flat;
    std::int64_t length = 0;
    std::int64_t null_count = 0;
    /// FLAT/CONSTANT values, the SELECTION/DICTIONARY index buffer (int32), or
    /// the byte data of a variable-width String/Binary column.
    std::shared_ptr<dftracer::utils::dataframe::Buffer> data;
    /// int32 offsets (length+1 entries) for a variable-width String/Binary/List
    /// column; null for fixed-width types and for the 64-bit-offset Large*
    /// variants, which use `offsets64` instead.
    std::shared_ptr<dftracer::utils::dataframe::Buffer> offsets;
    /// int64 offsets (length+1 entries) for LargeString/LargeBinary/LargeList,
    /// whose 64-bit-offset layout is a distinct physical type from String/
    /// Binary/List's 32-bit offsets; null otherwise. Not exposed through the
    /// public dftu_series_offsets ABI, which is int32-only.
    std::shared_ptr<dftracer::utils::dataframe::Buffer> offsets64;
    /// Arrow-layout validity bitmap (1 = valid); null when there are no nulls.
    std::shared_ptr<dftracer::utils::dataframe::Buffer> validity;
    /// Base for SELECTION/DICTIONARY, or the flattened values of a LIST column;
    /// null otherwise.
    std::shared_ptr<dftu_series> child;
    /// Field columns of a STRUCT (all length == this->length); empty otherwise.
    std::vector<std::shared_ptr<dftu_series>> children;
    /// Field names of a STRUCT, aligned to `children`.
    std::vector<std::string> field_names;
    /// Timestamp/Time32/Time64/Duration only.
    dftracer::utils::dataframe::TimeUnit time_unit =
        dftracer::utils::dataframe::TimeUnit::Micro;
    /// Timestamp only; empty means no timezone.
    std::string timezone;
    /// Decimal128/Decimal256 only.
    std::int32_t decimal_precision = 0;
    std::int32_t decimal_scale = 0;
    /// FixedSizeBinary (byte width) or FixedSizeList (element count) only.
    std::int32_t fixed_size = 0;
};

namespace dftracer {
namespace utils {
namespace dataframe {

/// Copies type and every type parameter (time_unit, timezone,
/// decimal_precision, decimal_scale, fixed_size) from src into out, leaving
/// out's data/length/encoding untouched. Use for any op whose result column
/// derives its type from a single source column (gather, slice, sort, unique,
/// reverse, fill_null, drop_nulls, dictionary_encode, ...).
inline void adopt_type_from(dftu_series& out, const dftu_series& src) {
    out.type = src.type;
    out.time_unit = src.time_unit;
    out.timezone = src.timezone;
    out.decimal_precision = src.decimal_precision;
    out.decimal_scale = src.decimal_scale;
    out.fixed_size = src.fixed_size;
}

/// Same as adopt_type_from, but sets out.type to an explicit TypeId while
/// still copying src's type parameters. Use for an op that changes the type
/// tag within the same parameterized family (e.g. casting Timestamp[us] to
/// Timestamp[ns] keeps the timezone; the caller sets time_unit separately).
inline void adopt_type_params_as(dftu_series& out, const dftu_series& src,
                                 TypeId type) {
    adopt_type_from(out, src);
    out.type = type;
}

}  // namespace dataframe
}  // namespace utils
}  // namespace dftracer

/// A kernel that reads a column's buffers takes FLAT input. Placed first in a
/// C entry point, this runs the same entry on the materialized copy of a view
/// (a SELECTION or DICTIONARY over a base) and frees the copy; a FLAT input
/// falls through. `fn` is the entry itself, `...` its remaining arguments.
#define DFTU_FLAT_INPUT(v, fn, ...)                                          \
    do {                                                                     \
        if ((v)->encoding != ::dftracer::utils::dataframe::Encoding::Flat) { \
            dftu_series* flat_ = dftu_series_materialize(v);                 \
            if (!flat_) return {};                                           \
            auto result_ = fn(flat_, ##__VA_ARGS__);                         \
            dftu_series_free(flat_);                                         \
            return result_;                                                  \
        }                                                                    \
    } while (0)

/// The same seam for a column operand in any position: `call` is the full
/// entry call spelled with `flat` where the operand `v` goes.
#define DFTU_FLAT_OPERAND(v, flat, call)                                     \
    do {                                                                     \
        if ((v)->encoding != ::dftracer::utils::dataframe::Encoding::Flat) { \
            dftu_series* flat = dftu_series_materialize(v);                  \
            if (!flat) return {};                                            \
            auto result_ = call;                                             \
            dftu_series_free(flat);                                          \
            return result_;                                                  \
        }                                                                    \
    } while (0)

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_COLUMN_DATA_H
