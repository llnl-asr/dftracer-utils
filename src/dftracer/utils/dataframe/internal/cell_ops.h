// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_CELL_OPS_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_CELL_OPS_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

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
inline dstd::String cell_to_string(const Series& c, std::int64_t i) {
    if (c.handle()->is_chunked())
        return at_chunk(c, i, [](const Series& k, std::int64_t j) {
            return cell_to_string(k, j);
        });
    switch (c.type()) {
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
        case TypeId::FixedSizeBinary:
            return dstd::String(read_bytes(c, i));
        case TypeId::Bool:
            return ((c.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1) ? "true"
                                                                     : "false";
        case TypeId::Int8:
            return dstd::to_string(c.data<std::int8_t>()[i]);
        case TypeId::Int16:
            return dstd::to_string(c.data<std::int16_t>()[i]);
        case TypeId::Int32:
            return dstd::to_string(c.data<std::int32_t>()[i]);
        case TypeId::Int64:
            return dstd::to_string(c.data<std::int64_t>()[i]);
        case TypeId::Uint8:
            return dstd::to_string(c.data<std::uint8_t>()[i]);
        case TypeId::Uint16:
            return dstd::to_string(c.data<std::uint16_t>()[i]);
        case TypeId::Uint32:
            return dstd::to_string(c.data<std::uint32_t>()[i]);
        case TypeId::Uint64:
            return dstd::to_string(c.data<std::uint64_t>()[i]);
        case TypeId::Float32:
            return dstd::to_string(c.data<float>()[i]);
        case TypeId::Float64:
        case TypeId::Float16:
        case TypeId::Decimal128:
        case TypeId::Decimal256:
            return dstd::to_string(read_f64(c, i));
        case TypeId::Date32:
        case TypeId::Date64:
        case TypeId::Time32:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
            return dstd::to_string(read_i64(c, i));
        default:
            return dstd::String();
    }
}

inline bool try_append_cell(dstd::String& key, const Series& c, std::int64_t i,
                            TypeId& refused);

/// A flat Struct cell keyed by field name, in name order, leaving out null
/// fields: two cells are equal when every field present in either is equal,
/// a field one of them lacks counting as null. False, with the field's type in
/// `refused`, when a field is a column @ref try_append_cell refuses.
inline bool try_append_struct_cell(dstd::String& key, const Series& c,
                                   std::int64_t i, TypeId& refused) {
    dstd::Vector<dstd::Pair<dstd::String, Series>> fields;
    for (std::int64_t f = 0; f < c.num_children(); ++f)
        fields.emplace_back(c.field_name(f), c.child(f));
    std::sort(fields.begin(), fields.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    key.push_back('\2');
    for (const auto& [name, field] : fields) {
        if (field.is_null(i)) continue;
        const auto len = static_cast<std::int32_t>(name.size());
        key.append(reinterpret_cast<const char*>(&len), sizeof(len));
        key.append(name);
        if (!try_append_cell(key, field, i, refused)) return false;
    }
    key.push_back('\3');
    return true;
}

/// Append an exact byte encoding of cell `i` of `c` to `key`: a null flag then
/// the raw bytes (length-prefixed for byte-domain cells), so distinct cells
/// never collide and equal cells always match. The building block for row
/// dedupe keys. A flat Struct keys by field name (try_append_struct_cell).
/// Returns false, with the column type in `refused`, for any other nested
/// column; `key` is then unfinished.
inline bool try_append_cell(dstd::String& key, const Series& c, std::int64_t i,
                            TypeId& refused) {
    if (c.type() == TypeId::Struct && c.is_flat()) {
        if (c.is_null(i)) {
            key.push_back('\0');
            return true;
        }
        return try_append_struct_cell(key, c, i, refused);
    }
    if (!is_orderable_type(c.type())) {
        refused = c.type();
        return false;
    }
    if (c.is_null(i)) {
        key.push_back('\0');
        return true;
    }
    if (c.handle()->is_chunked()) {
        bool ok = true;
        at_chunk(c, i, [&](const Series& k, std::int64_t j) {
            ok = try_append_cell(key, k, j, refused);
            return 0;
        });
        return ok;
    }
    key.push_back('\1');
    const TypeId t = c.type();
    if (value_domain(t) == ValueDomain::Bytes) {
        const dstd::StringView s = read_bytes(c, i);
        const auto len = static_cast<std::int32_t>(s.size());
        key.append(reinterpret_cast<const char*>(&len), sizeof(len));
        key.append(s.data(), s.size());
        return true;
    }
    const auto* d =
        static_cast<const std::uint8_t*>(dftu_series_data(c.handle()));
    if (t == TypeId::Bool) {
        key.push_back((d[i >> 3] >> (i & 7)) & 1 ? '\1' : '\0');
        return true;
    }
    const std::size_t w = byte_width(t).value_or(0);
    key.append(
        reinterpret_cast<const char*>(d + static_cast<std::size_t>(i) * w), w);
    return true;
}

/// The error for a column type @ref try_append_cell refused.
inline dstd::Unexpected<dstd::Error> cell_key_error(TypeId refused) {
    return common::make_error<
        "column type '{}' has no per-row value to key on">(
        common::ErrorCode::InvalidArgument, type_name(refused));
}

/// Exact dedupe key of row `i` across `cols` (already FLAT), appended to `k`.
/// False, with the column type in `refused`, when a column has no per-row key.
inline bool try_row_key(const dstd::Vector<Series>& cols, std::int64_t i,
                        dstd::String& k, TypeId& refused) {
    for (const Series& c : cols)
        if (!try_append_cell(k, c, i, refused)) return false;
    return true;
}

/// The first column type a key build refused. A loop over rows builds keys
/// with the slot forms below and reads this once after the loop, so a row
/// costs no result check. One per call, never shared across threads.
class KeyRefusal {
   public:
    void set(TypeId t) noexcept {
        if (!refused_) {
            refused_ = true;
            type_ = t;
        }
    }
    bool refused() const noexcept { return refused_; }
    /// Keeps the refusal of `other` unless this one holds one.
    void merge(const KeyRefusal& other) noexcept {
        if (other.refused_) set(other.type_);
    }
    /// The error for the first refusal; only after @ref refused is true.
    dstd::Unexpected<dstd::Error> error() const {
        return cell_key_error(type_);
    }

   private:
    TypeId type_ = TypeId::Unknown;
    bool refused_ = false;
};

/// A @ref KeyRefusal that the chunks of one parallel loop report into: each
/// chunk keeps its own @ref KeyRefusal and merges it here once, only when it
/// holds a refusal. The caller reads @ref get after the loop.
class ConcurrentKeyRefusal {
   public:
    void merge(const KeyRefusal& chunk) {
        if (!chunk.refused()) return;
        std::lock_guard<std::mutex> lock(mu_);
        slot_.merge(chunk);
    }
    const KeyRefusal& get() const noexcept { return slot_; }

   private:
    std::mutex mu_;
    KeyRefusal slot_;
};

/// @ref try_append_cell with the refusal kept in `refusal`; `key` is then
/// unfinished and the caller must not use it.
inline void append_cell(dstd::String& key, const Series& c, std::int64_t i,
                        KeyRefusal& refusal) {
    TypeId refused = TypeId::Unknown;
    if (!try_append_cell(key, c, i, refused)) refusal.set(refused);
}

/// Exact dedupe key of row `i` across `cols` (already FLAT). A column with no
/// per-row key is recorded in `refusal` and the key is unfinished.
inline dstd::String row_key(const dstd::Vector<Series>& cols, std::int64_t i,
                            KeyRefusal& refusal) {
    dstd::String k;
    TypeId refused = TypeId::Unknown;
    if (!try_row_key(cols, i, k, refused)) refusal.set(refused);
    return k;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_CELL_OPS_H
