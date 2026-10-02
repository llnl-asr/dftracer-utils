#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_COLUMN_DATA_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_COLUMN_DATA_H

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/buffer.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/types.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct dftu_series;

/// Whether `total` bytes of a String or Binary column fit its int32 offsets.
inline bool fits_int32_offsets(std::uint64_t total) noexcept {
    return total <=
           static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
}

/// Turns the row lengths in off[1..n] into offsets with off[0] = 0. False,
/// with `off` left unusable, when a running total passes what `Off` holds.
template <class Off>
inline bool offsets_from_lengths(Off* off, std::int64_t n) noexcept {
    std::int64_t run = 0;
    off[0] = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        run += off[i + 1];
        if (run > static_cast<std::int64_t>(std::numeric_limits<Off>::max()))
            return false;
        off[i + 1] = static_cast<Off>(run);
    }
    return true;
}

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
    /// FLAT/CONSTANT values, the SELECTION (int64) or DICTIONARY (int32) index
    /// buffer, the byte data of a variable-width String/Binary column, or the
    /// CHUNKED chunk starts (int64, one per chunk plus the length).
    std::shared_ptr<dftracer::utils::dataframe::Buffer> data;
    /// Offsets (length+1 entries) of a variable-width column: int32 for
    /// String, Binary, List and Map; int64 for LargeString, LargeBinary and
    /// LargeList. Null for a fixed-width type.
    std::shared_ptr<dftracer::utils::dataframe::Buffer> offsets;
    /// Arrow-layout validity bitmap (1 = valid); null when there are no nulls.
    std::shared_ptr<dftracer::utils::dataframe::Buffer> validity;
    /// VIEW: the data buffers the 16-byte views in `data` point into, shared
    /// by every column made from this one. Null for any other encoding.
    std::shared_ptr<
        const std::vector<std::shared_ptr<dftracer::utils::dataframe::Buffer>>>
        blobs;
    /// A flat Struct's fields, in order; a CHUNKED column's chunks, in order;
    /// otherwise at most one entry, the base of a SELECTION or DICTIONARY or
    /// the values of a list-like column.
    std::vector<dftu_nested> nested;
    dftracer::utils::dataframe::TypeParams params;

    /// Whether `nested` holds Struct fields.
    bool has_fields() const noexcept {
        return type == dftracer::utils::dataframe::TypeId::Struct &&
               encoding == dftracer::utils::dataframe::Encoding::Flat;
    }
    /// The one child, null for a Struct's fields, a CHUNKED column's chunks or
    /// none.
    const std::shared_ptr<dftu_series>& child() const noexcept {
        static const std::shared_ptr<dftu_series> NONE;
        return nested.empty() || has_fields() || is_chunked()
                   ? NONE
                   : nested.front().series;
    }
    bool is_chunked() const noexcept {
        return encoding == dftracer::utils::dataframe::Encoding::Chunked;
    }
    /// CHUNKED: the chunk holding row `i`; `local` gets the row inside it.
    const dftu_series& chunk_at(std::int64_t i, std::int64_t& local) const {
        const auto* starts =
            reinterpret_cast<const std::int64_t*>(data->data());
        const std::size_t k = static_cast<std::size_t>(
            std::upper_bound(starts, starts + nested.size() + 1, i) - starts -
            1);
        local = i - starts[k];
        return *nested[k].series;
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
    /// Whether a row's nullness needs `dftu_series_is_null` rather than the
    /// own bitmap: a SELECTION, a CHUNKED column with nulls, or a DICTIONARY
    /// with null entries.
    bool rowwise_nulls() const noexcept {
        return encoding == dftracer::utils::dataframe::Encoding::Selection ||
               (is_chunked() && null_count > 0) ||
               (encoding == dftracer::utils::dataframe::Encoding::Dictionary &&
                child() && child()->null_count > 0);
    }
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

/// A CHUNKED column over `chunks` (at least two, same type, none CHUNKED).
inline dftu_series* make_chunked(
    std::vector<std::shared_ptr<dftu_series>> chunks) {
    auto* out = new dftu_series();
    adopt_type_from(*out, *chunks.front());
    out->encoding = Encoding::Chunked;
    out->data = Buffer::allocate((chunks.size() + 1) * sizeof(std::int64_t));
    auto* starts = reinterpret_cast<std::int64_t*>(out->data->data());
    std::int64_t at = 0;
    for (std::size_t k = 0; k < chunks.size(); ++k) {
        starts[k] = at;
        at += chunks[k]->length;
        out->null_count += dftu_series_null_count(chunks[k].get());
    }
    starts[chunks.size()] = at;
    out->length = at;
    out->nested.reserve(chunks.size());
    for (auto& c : chunks) out->nested.push_back({std::move(c), {}});
    return out;
}

}  // namespace dataframe
}  // namespace utils
}  // namespace dftracer

/// The per-chunk results of a row-wise kernel as one CHUNKED column (joined
/// when their types differ). Takes ownership of `parts`.
dftu_series* dftu_chunk_results(std::vector<dftu_series*>& parts);

/// `fn(chunk)` on each chunk of the CHUNKED `v`, run in parallel, the results
/// kept as chunks. Null when any chunk's result is null.
template <class F>
dftu_series* dftu_map_chunks(const dftu_series& v, F&& fn) {
    std::vector<dftu_series*> parts(v.nested.size(), nullptr);
    dftracer::utils::dataframe::parallel_for(
        static_cast<std::int64_t>(parts.size()), 1,
        [&](std::int64_t b, std::int64_t e) {
            for (std::int64_t k = b; k < e; ++k)
                parts[static_cast<std::size_t>(k)] =
                    fn(v.nested[static_cast<std::size_t>(k)].series.get());
        });
    for (dftu_series* p : parts)
        if (!p) {
            for (dftu_series* q : parts)
                if (q) dftu_series_free(q);
            return nullptr;
        }
    return dftu_chunk_results(parts);
}

/// The seam for a row-wise kernel (output row i reads only input row i): on a
/// CHUNKED `v`, the entry runs on each chunk and the results are its chunks.
/// Placed first in the entry point, before DFTU_FLAT_INPUT. `fn` is the entry
/// itself,
/// `...` its remaining arguments.
#define DFTU_PER_CHUNK(v, fn, ...)                                      \
    do {                                                                \
        if ((v)->is_chunked())                                          \
            return ::dftu_map_chunks(*(v), [&](const dftu_series* c_) { \
                return fn(c_, ##__VA_ARGS__);                           \
            });                                                         \
    } while (0)

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
