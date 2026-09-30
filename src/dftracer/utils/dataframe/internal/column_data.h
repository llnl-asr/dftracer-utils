#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_COLUMN_DATA_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_COLUMN_DATA_H

#include <dftracer/utils/dataframe/buffer.h>
#include <dftracer/utils/dataframe/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct dftu_series;

/// A nested column of a dftu_series: a Struct field with its name, or the
/// one child of a list-like, SELECTION or DICTIONARY column (no name).
struct dftu_nested {
    std::shared_ptr<dftu_series> series;
    std::string name;
};

/// Layout behind the opaque dftu_series handle. Private to the dataframe
/// sources; the public headers only forward-declare it. Members that only
/// some types use share storage keyed by `type` and `encoding`.
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
    /// Offsets (length+1 entries) of a variable-width column: int32 for
    /// String, Binary, List and Map; int64 for LargeString, LargeBinary and
    /// LargeList. Null for a fixed-width type.
    std::shared_ptr<dftracer::utils::dataframe::Buffer> offsets;
    /// Arrow-layout validity bitmap (1 = valid); null when there are no nulls.
    std::shared_ptr<dftracer::utils::dataframe::Buffer> validity;
    /// A flat Struct's fields, in order; otherwise at most one entry, the
    /// base of a SELECTION or DICTIONARY or the values of a list-like column.
    std::vector<dftu_nested> nested;
    dftracer::utils::dataframe::TypeParams params;

    /// Whether `nested` holds Struct fields.
    bool has_fields() const noexcept {
        return type == dftracer::utils::dataframe::TypeId::Struct &&
               encoding == dftracer::utils::dataframe::Encoding::Flat;
    }
    /// The one child, null for a Struct's fields or none.
    const std::shared_ptr<dftu_series>& child() const noexcept {
        static const std::shared_ptr<dftu_series> NONE;
        return nested.empty() || has_fields() ? NONE : nested.front().series;
    }
    void set_child(std::shared_ptr<dftu_series> c) {
        nested.clear();
        if (c) nested.push_back({std::move(c), {}});
    }
    std::size_t num_fields() const noexcept {
        return has_fields() ? nested.size() : 0;
    }

    dftracer::utils::dataframe::TimeUnit time_unit() const noexcept {
        return params.time_unit(type);
    }
    std::string_view timezone() const { return params.timezone(type); }
    std::int32_t decimal_precision() const noexcept {
        return params.decimal_precision(type);
    }
    std::int32_t decimal_scale() const noexcept {
        return params.decimal_scale(type);
    }
    std::int32_t fixed_size() const noexcept { return params.fixed_size(type); }
    bool json() const noexcept { return params.is_json(type); }
    /// Set `type` first: a parameter the type does not use is ignored.
    void set_time_unit(dftracer::utils::dataframe::TimeUnit unit) noexcept {
        params.set_time_unit(type, unit);
    }
    void set_timezone(std::string_view zone) {
        params.set_timezone(type, zone);
    }
    void set_decimal(std::int32_t precision, std::int32_t scale) noexcept {
        params.set_decimal(type, precision, scale);
    }
    void set_fixed_size(std::int32_t n) noexcept {
        params.set_fixed_size(type, n);
    }
    void set_json(bool json) noexcept { params.set_json(type, json); }
    /// The offsets are int64.
    bool wide_offsets() const noexcept {
        using dftracer::utils::dataframe::TypeId;
        return type == TypeId::LargeString || type == TypeId::LargeBinary ||
               type == TypeId::LargeList;
    }
};

namespace dftracer {
namespace utils {
namespace dataframe {

/// Copies type and every type parameter from src into out, leaving out's
/// data/length/encoding untouched. Use for any op whose result column derives
/// its type from a single source column (gather, slice, sort, unique,
/// reverse, fill_null, drop_nulls, dictionary_encode, ...).
inline void adopt_type_from(dftu_series& out, const dftu_series& src) {
    out.type = src.type;
    out.params = src.params;
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
