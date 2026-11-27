// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_SERIES_H
#define DFTRACER_UTILS_DATAFRAME_SERIES_H

#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/shared_string.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe {

struct DataFrame;

/// The rows that label keys select, from @ref Series::locate or
/// @ref DataFrame::locate.
struct Located {
    /// The row positions in key order: every row of the first key that has
    /// rows, then those of the next (a label held by several rows gives them
    /// all, in row order; a label repeated in the keys is repeated).
    dstd::Vector<std::int64_t> positions;
    /// The key rows that no label matches, ascending; `positions` has nothing
    /// for them.
    dstd::Vector<std::int64_t> missing;
};

/// A run of rows, `length` rows from `offset`.
struct LabelSpan {
    std::int64_t offset = 0;
    std::int64_t length = 0;
};

/// How @ref Series::reindex fills a label that has no row, as the pandas
/// `method`: the row of the nearest label before it (Pad, `ffill`), after it
/// (Backfill, `bfill`) or on either side (Nearest; a tie takes the larger
/// label). Before and after are in label order, whether the labels increase or
/// decrease.
enum class FillMethod : std::uint8_t { None, Pad, Backfill, Nearest };

/// The options of @ref Series::reindex and @ref DataFrame::reindex. With no
/// method, a label with no row gets a null and `limit` and `tolerance` must be
/// left unset.
struct ReindexOptions {
    FillMethod method = FillMethod::None;
    /// The most target labels that one row fills, counting only the labels it
    /// fills (not an exact match), or negative for no limit. A limit needs
    /// labels that increase and target labels that do not decrease.
    std::int64_t limit = -1;
    /// The farthest a fill may reach, `|target - label| <= tolerance`, in the
    /// units of the label (a timestamp counts its ticks), or infinite for no
    /// limit. Labels must be numbers or temporal values. Integer and temporal
    /// labels are compared exactly (a count above 2**53 is not rounded), floats
    /// in double precision.
    double tolerance = std::numeric_limits<double>::infinity();
    /// For Timestamp, Duration and Time labels, a tolerance as whole
    /// nanoseconds, exact in any unit of the labels and keys (`tolerance` is
    /// then ignored). An InvalidArgument error for labels with no time unit.
    bool tolerance_is_nanos = false;
    std::int64_t tolerance_nanos = 0;
};

/// A typed column. Move-only owning wrapper over the C ABI handle; the buffers,
/// encoding, and null handling live behind it. Typed accessors are inline
/// (zero-cost) for in-tree callers; the C ABI is the stable cross-boundary
/// seam.
class Series {
   public:
    Series() = default;
    explicit Series(dftu_series* handle) noexcept : handle_(handle) {}

    ~Series() {
        if (handle_ != nullptr) dftu_series_free(handle_);
    }

    Series(Series&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    Series& operator=(Series&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr) dftu_series_free(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    Series(const Series&) = delete;
    Series& operator=(const Series&) = delete;

    /// Build a FLAT column copying `n` fixed-width values of `type` from
    /// `data`. `validity` is an Arrow-layout bitmap (1 = valid) or null for no
    /// nulls.
    static Series flat(TypeId type, const void* data, std::int64_t n,
                       const std::uint8_t* validity = nullptr) {
        return Series{dftu_series_new_flat(static_cast<dftu_dtype>(type), data,
                                           n, validity)};
    }
    static Series flat_i64(const std::int64_t* data, std::int64_t n,
                           const std::uint8_t* validity = nullptr) {
        return flat(TypeId::Int64, data, n, validity);
    }
    static Series flat_f64(const double* data, std::int64_t n,
                           const std::uint8_t* validity = nullptr) {
        return flat(TypeId::Float64, data, n, validity);
    }

    /// Build a FLAT column BORROWING `n` fixed-width values of `type` from
    /// `data` with NO copy: the column's data buffer points straight at `data`.
    /// The caller must keep `data` alive until `release(release_ctx)` runs,
    /// which happens exactly once when the column's last reference is dropped
    /// (Series destruction, possibly off the creating thread). Pass a null
    /// `release` to borrow static or otherwise-owned memory. `validity` is
    /// borrowed like `data`: it must live as long as `release` has not run.
    /// Returns an invalid Series for a variable-width type (String/Binary),
    /// where borrowing a single flat buffer is not meaningful. Mirrors
    /// dftu_series_new_flat_borrowed.
    static Series from_borrowed(TypeId type, const void* data, std::int64_t n,
                                const std::uint8_t* validity = nullptr,
                                void (*release)(void* ctx) = nullptr,
                                void* release_ctx = nullptr) {
        return Series{
            dftu_series_new_flat_borrowed(static_cast<dftu_dtype>(type), data,
                                          n, validity, release, release_ctx)};
    }

    /// Build a FLAT column BORROWING `data` and taking ownership of `owner`.
    /// `data` must point into storage kept alive by `owner` (a std::vector, a
    /// std::unique_ptr, a std::shared_ptr, a buffer view, ...); the column
    /// borrows the buffer with no copy and destroys `owner` exactly once when
    /// its last reference is dropped, freeing whatever backs `data`. On a
    /// variable-width `type` the borrow is rejected and `owner` is destroyed
    /// before an invalid Series is returned. `validity` is COPIED, into a
    /// bitmap released with `owner`, so a caller's local bitmap is safe here
    /// (dftu_series_new_flat_borrowed itself borrows it).
    template <class Owner, class = std::enable_if_t<!std::is_convertible_v<
                               Owner&&, const std::uint8_t*>>>
    static Series from_borrowed(TypeId type, const void* data, std::int64_t n,
                                Owner&& owner,
                                const std::uint8_t* validity = nullptr) {
        using Held = std::decay_t<Owner>;
        struct Bundle {
            Held owner;
            dstd::Vector<std::uint8_t> validity;
        };
        auto* held = new Bundle{std::forward<Owner>(owner), {}};
        if (validity != nullptr && n > 0) {
            held->validity.resize(static_cast<std::size_t>((n + 7) / 8));
            std::memcpy(held->validity.data(), validity, held->validity.size());
        }
        // A rejected borrow (variable-width) runs release() itself, so `held`
        // is freed there; never double-free it here.
        return Series{dftu_series_new_flat_borrowed(
            static_cast<dftu_dtype>(type), data, n,
            held->validity.empty() ? nullptr : held->validity.data(),
            [](void* ctx) { delete static_cast<Bundle*>(ctx); }, held)};
    }

    /// Ends the process when `total` bytes do not fit the int32 offsets of a
    /// String column.
    static void check_string_bytes(std::size_t total) {
        if (total >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            dstd::fatal("String column exceeds INT32_MAX bytes");
    }

    /// Build a FLAT String column from `values` (offsets + concatenated bytes).
    /// Ends the process when the total bytes pass INT32_MAX.
    static Series strings(dstd::Span<const dstd::String> values) {
        dstd::Vector<std::int32_t> offsets(values.size() + 1, 0);
        std::size_t total = 0;
        for (const auto& v : values) total += v.size();
        check_string_bytes(total);
        dstd::String data;
        for (std::size_t i = 0; i < values.size(); ++i) {
            data += values[i];
            offsets[i + 1] = static_cast<std::int32_t>(data.size());
        }
        return Series{dftu_series_new_string(
            static_cast<dftu_dtype>(TypeId::String), offsets.data(),
            data.data(), static_cast<std::int64_t>(values.size()), nullptr)};
    }

    /// Build a FLAT String column from `values`. Copies the bytes once into the
    /// contiguous Arrow buffer, without the intermediate std::string vector the
    /// overload above builds.
    static Series strings(dstd::Span<const dstd::StringView> values) {
        dstd::Vector<std::int32_t> offsets(values.size() + 1, 0);
        std::size_t total = 0;
        for (const auto& v : values) total += v.size();
        check_string_bytes(total);
        dstd::String data;
        data.reserve(total);
        for (std::size_t i = 0; i < values.size(); ++i) {
            data.append(values[i].data(), values[i].size());
            offsets[i + 1] = static_cast<std::int32_t>(data.size());
        }
        return Series{dftu_series_new_string(
            static_cast<dftu_dtype>(TypeId::String), offsets.data(),
            data.data(), static_cast<std::int64_t>(values.size()), nullptr)};
    }

    /// Build a FLAT String column with a validity bitmap (Arrow layout, 1 =
    /// valid). A null slot contributes no bytes (its offset repeats the prior).
    static Series strings(dstd::Span<const dstd::StringView> values,
                          const std::uint8_t* validity) {
        dstd::Vector<std::int32_t> offsets(values.size() + 1, 0);
        std::size_t total = 0;
        for (const auto& v : values) total += v.size();
        check_string_bytes(total);
        dstd::String data;
        data.reserve(total);
        for (std::size_t i = 0; i < values.size(); ++i) {
            data.append(values[i].data(), values[i].size());
            offsets[i + 1] = static_cast<std::int32_t>(data.size());
        }
        return Series{dftu_series_new_string(
            static_cast<dftu_dtype>(TypeId::String), offsets.data(),
            data.data(), static_cast<std::int64_t>(values.size()), validity)};
    }

    /// An all-null column of `type` and length `n` (validity all-zero). Used to
    /// fill a column absent from one part of a schema-union concat.
    static Series nulls(TypeId type, std::int64_t n) {
        if (n < 0) n = 0;
        dstd::Vector<std::uint8_t> validity(
            static_cast<std::size_t>((n + 7) / 8), 0);  // every row null
        if (type == TypeId::String || type == TypeId::Binary) {
            dstd::Vector<std::int32_t> offsets(static_cast<std::size_t>(n) + 1,
                                               0);
            return Series{dftu_series_new_string(static_cast<dftu_dtype>(type),
                                                 offsets.data(), "", n,
                                                 validity.data())};
        }
        dstd::Vector<std::uint8_t> data(buffer_bytes(type, n), 0);
        return Series{dftu_series_new_flat(static_cast<dftu_dtype>(type),
                                           data.data(), n, validity.data())};
    }

    /// Build a STRUCT column from field `names` and `columns` (each the same
    /// length, aligned to names); takes ownership of the field columns. The
    /// columns are move-only, so pass them in a vector built by move.
    static Series structs(dstd::Span<const dstd::String> names,
                          dstd::Vector<Series> columns);

    /// @ref structs over shared names: the names are read, not copied.
    static Series structs(dstd::Span<const dstd::SharedString> names,
                          dstd::Vector<Series> columns);

    /// @ref structs over literal names, such as `structs({"a", "b"}, ...)`.
    static Series structs(std::initializer_list<const char*> names,
                          dstd::Vector<Series> columns);

    /// Build a LIST column: `offsets` (n+1 entries) index into `values`; takes
    /// ownership of `values`.
    static Series list(dstd::Span<const std::int32_t> offsets, Series values) {
        return Series{dftu_series_new_list(
            offsets.data(), static_cast<std::int64_t>(offsets.size()) - 1,
            values.release())};
    }

    bool valid() const noexcept { return handle_ != nullptr; }
    dftu_series* handle() const noexcept { return handle_; }
    /// Relinquish ownership of the handle (for transferring into a nested
    /// column); the Series becomes empty.
    dftu_series* release() noexcept {
        dftu_series* h = handle_;
        handle_ = nullptr;
        return h;
    }

    TypeId type() const noexcept {
        return static_cast<TypeId>(dftu_series_type(handle_));
    }
    Encoding encoding() const noexcept {
        return static_cast<Encoding>(dftu_series_encoding(handle_));
    }
    std::int64_t length() const noexcept { return dftu_series_length(handle_); }
    std::int64_t null_count() const noexcept {
        return dftu_series_null_count(handle_);
    }
    /// Whether row `i` is null (a column with no validity is all-valid).
    bool is_null(std::int64_t i) const noexcept {
        return dftu_series_is_null(handle_, i) != 0;
    }

    /// Raw FLAT value buffer as `T*`; null unless the column is FLAT.
    template <class T>
    const T* data() const noexcept {
        return static_cast<const T*>(dftu_series_data(handle_));
    }

    /// FLAT value buffer as a span of `length()` elements; empty unless FLAT.
    template <class T>
    dstd::Span<const T> values() const noexcept {
        const T* p = data<T>();
        return p != nullptr
                   ? dstd::Span<const T>(p, static_cast<std::size_t>(length()))
                   : dstd::Span<const T>();
    }

    /// Value of a String/Binary/LargeString/LargeBinary column at `i`, valid
    /// while this column lives. Reads FLAT, DICTIONARY, SELECTION and VIEW
    /// columns;
    /// empty for a null or out-of-range row or a non-string column.
    dstd::StringView string_at(std::int64_t i) const noexcept {
        if (!is_flat()) {
            std::int64_t n = 0;
            const char* p = dftu_series_string_at(handle_, i, &n);
            return p != nullptr
                       ? dstd::StringView(p, static_cast<std::size_t>(n))
                       : dstd::StringView();
        }
        const char* d = static_cast<const char*>(dftu_series_data(handle_));
        if (d == nullptr) return {};
        if (is_wide_offset_type(type())) {
            const std::int64_t* off = dftu_series_offsets64(handle_);
            if (off == nullptr) return {};
            return dstd::StringView(
                d + off[i], static_cast<std::size_t>(off[i + 1] - off[i]));
        }
        const std::int32_t* off = dftu_series_offsets(handle_);
        if (off == nullptr) return {};
        return dstd::StringView(d + off[i],
                                static_cast<std::size_t>(off[i + 1] - off[i]));
    }

    /// Whether the values live in this column's own buffers, so `data()`
    /// and `offsets()` can read them. False for CONSTANT,
    /// DICTIONARY, SELECTION and VIEW, which `materialize()` converts.
    bool is_flat() const noexcept { return encoding() == Encoding::Flat; }

    /// int32 offset buffer (length+1 entries) of a String/Binary or List
    /// column, or null for fixed-width types and for a LargeString/
    /// LargeBinary/LargeList column (see `offsets64()`).
    const std::int32_t* offsets() const noexcept {
        return dftu_series_offsets(handle_);
    }

    /// int32 offset buffer as a span of `length()+1` entries; empty for
    /// fixed-width types and for a Large* column.
    dstd::Span<const std::int32_t> offsets_span() const noexcept {
        const std::int32_t* off = offsets();
        return off != nullptr ? dstd::Span<const std::int32_t>(
                                    off, static_cast<std::size_t>(length()) + 1)
                              : dstd::Span<const std::int32_t>();
    }

    /// int64 offset buffer (length+1 entries) of a LargeString/LargeBinary/
    /// LargeList column, or null otherwise.
    const std::int64_t* offsets64() const noexcept {
        return dftu_series_offsets64(handle_);
    }

    /// int64 offset buffer as a span of `length()+1` entries; empty unless
    /// this is a Large* column.
    dstd::Span<const std::int64_t> offsets64_span() const noexcept {
        const std::int64_t* off = offsets64();
        return off != nullptr ? dstd::Span<const std::int64_t>(
                                    off, static_cast<std::size_t>(length()) + 1)
                              : dstd::Span<const std::int64_t>();
    }

    /// Child count: 1 for a List, the field count for a Struct, else 0.
    std::int64_t num_children() const noexcept {
        return dftu_series_num_children(handle_);
    }

    /// Child column `i` (List: 0 = values; Struct: field i). Invalid if out of
    /// range.
    Series child(std::int64_t i) const noexcept {
        return Series{dftu_series_child(handle_, static_cast<std::int32_t>(i))};
    }

    /// Name of Struct field `i`; empty for a List's element or out of range.
    dstd::String field_name(std::int64_t i) const {
        const char* s =
            dftu_series_field_name(handle_, static_cast<std::int32_t>(i));
        return s ? dstd::String(s) : dstd::String();
    }

    /// The full nested @ref DataType, walking `num_children()`/`child()` and
    /// each Struct field's name. A List's single child becomes its (unnamed)
    /// element Field; a Struct's children become named Fields in order.
    DataType data_type() const;

    /// The series name, valid while this column lives; empty when it has none.
    /// @ref share, @ref slice and @ref as_json keep it. A method that derives a
    /// column from this one elementwise or as a transform (a unary op, cast,
    /// fillna, rolling, str ops, filter, take, sort, argsort) gives the result
    /// this name, as pandas does. A binary op between two columns keeps it only
    /// when both names are equal. A search_sorted and a @ref value_counts
    /// result have no name.
    dstd::StringView name() const noexcept {
        std::size_t n = 0;
        const char* p = dftu_series_name(handle_, &n);
        return p != nullptr ? dstd::StringView(p, n) : dstd::StringView();
    }

    /// A new owned column sharing this column's buffers zero-copy, named
    /// @p name; an empty @p name clears the name. Invalid if this column is.
    Series rename(dstd::StringView name) const {
        return Series{dftu_series_rename(handle_, name.data(), name.size())};
    }
    /// Like the view overload, but shares @p name's bytes and allocates none.
    Series rename(const dstd::SharedString& name) const;

    /// A new owned column sharing this column's buffers zero-copy (refcount
    /// bump, no data copy).
    Series share() const noexcept { return Series{dftu_series_share(handle_)}; }

    /// The number of row-label levels; 0 for a positional column (the pandas
    /// RangeIndex, nothing materialized). The labels are ordinary columns as
    /// long as this one. @ref share, @ref rename, @ref slice and @ref as_json
    /// keep them. A method that keeps the rows in place (a unary op, cast,
    /// fillna, cumsum, shift, rank, scalar arithmetic) keeps them, and one that
    /// selects or reorders rows (@ref sort, @ref head, @ref tail, @ref filter,
    /// @ref take, @ref drop_nulls, @ref top_k, @ref bottom_k, @ref sample,
    /// @ref reverse) moves them with the rows. A reducer, @ref unique and
    /// @ref value_counts give none. Labels match by value: see @ref
    /// align_labels.
    std::int32_t index_count() const noexcept {
        return dftu_series_index_count(handle_);
    }
    bool has_index() const noexcept { return index_count() > 0; }
    /// Name of label level `i`, valid while this column lives; empty out of
    /// range.
    dstd::StringView index_name(std::int32_t i) const noexcept {
        const char* p = nullptr;
        std::size_t n = 0;
        return dftu_series_index_name(handle_, i, &p, &n) == 0
                   ? dstd::StringView(p, n)
                   : dstd::StringView();
    }
    /// Label column of level `i`, sharing its buffers and carrying no labels
    /// of its own; invalid out of range.
    Series index_level(std::int32_t i) const {
        return Series{dftu_series_index_column(handle_, i)};
    }
    /// A zero-copy copy of this column whose labels are `levels` named
    /// `names`; empty spans drop them. Invalid when the spans differ in size, a
    /// level is not as long as this column, a level is a List or Struct, or a
    /// name repeats.
    Series with_index(dstd::Span<const dstd::StringView> names,
                      dstd::Span<const Series> levels) const;
    /// A zero-copy copy of this column with no labels.
    Series without_index() const;
    /// This column under the labels of `source`, shared, not copied; the column
    /// unchanged when `source` has none or has another length.
    Series with_index_of(const Series& source) const;
    /// The label levels as a frame, one column per level in order, whose row
    /// index is all of them; an empty frame when there are none.
    DataFrame index_frame() const;
    /// The rows whose labels equal `keys`, as pandas `loc` finds them. `keys`
    /// holds one column per label level, the leading levels when it holds fewer
    /// than this column has (a partial key of a MultiIndex), each as long as
    /// the number of keys: row `r` of every column is key `r`. A positional
    /// column has one level, the positions 0 .. length-1. Labels compare by
    /// value: an integer key finds a float label of the same value, a null key
    /// finds the null labels (and a NaN key, which is a null label), a string
    /// key finds no number. A key of a type that has no label of that kind
    /// finds nothing. The lookup hashes the labels once, on the first call, and
    /// keeps the hash with the labels (shared by every copy of this column,
    /// built once even when several threads call at once; a later call costs
    /// the keys only). An InvalidArgument error for no keys, more key levels
    /// than this column has labels, keys of different lengths or a List or
    /// Struct key.
    dstd::Result<Located> locate(dstd::Span<const Series> keys) const;
    /// The rows between two label bounds, both ends included, as pandas
    /// `loc[start:stop]`. `start` and `stop` are one-row columns per level,
    /// leading levels as in @ref locate; an empty span is an open end. On a
    /// monotonic index (increasing or decreasing, lexicographic across levels,
    /// no null label) a bound need not be a label: the run is found by binary
    /// search. On any other index a bound must be one label that no other row
    /// has, and the run is the rows from the start row to the stop row (empty
    /// when the stop row is first), and a MultiIndex that is not monotonic is
    /// refused. A NotFound error naming the bound for a missing or repeated
    /// bound, or the unsorted MultiIndex; an InvalidArgument error for bounds
    /// of a type that does not compare with the labels.
    dstd::Result<LabelSpan> locate_range(dstd::Span<const Series> start,
                                         dstd::Span<const Series> stop) const;
    /// This column on the labels `keys` as pandas `reindex`: row `r` of the
    /// result is the row whose label equals key `r`, and a null where none
    /// does (or the row @p options names). `keys` holds one column per label
    /// level, as long as the number of keys; a positional column has the one
    /// level 0 .. length-1. The result carries `keys` as its labels, under the
    /// level names of this column (the default `__index_level_<i>__` when it
    /// has none). An integer column that gains nulls becomes Float64. With a
    /// one-row @p fill, a row with no label takes that value instead: the
    /// column keeps its type when the value fits it (an integer in range for an
    /// integer column, any number for a float one, a string for a string
    /// column) and widens to Float64 for a float value in an integer column; a
    /// null the column already held stays. Labels that are the same as the
    /// keys, row for row, return the column unchanged even when they repeat.
    /// Otherwise an InvalidArgument error for repeated labels ("cannot reindex
    /// on an axis with duplicate labels"), a key level count other than the
    /// label level count, a @p fill that does not fit the type, a limit or
    /// tolerance with no method, a method on labels that are not monotonic
    /// ("index must be monotonic increasing or decreasing"), a limit on labels
    /// that do not increase or targets that decrease, a tolerance or the
    /// nearest method on labels that do not subtract (text) or on more than one
    /// level.
    dstd::Result<Series> reindex(dstd::Span<const Series> keys,
                                 const ReindexOptions& options,
                                 const Series& fill) const;
    /// @ref reindex with no fill value.
    dstd::Result<Series> reindex(dstd::Span<const Series> keys,
                                 const ReindexOptions& options = {}) const;

    /// Whether this is a JSON column: String values of canonical JSON text.
    bool is_json() const noexcept { return dftu_series_is_json(handle_) != 0; }

    /// This String column as a JSON column, sharing its buffers; the caller
    /// asserts each value is canonical JSON text. Invalid for another type.
    Series as_json() const noexcept {
        return Series{dftu_series_mark_json(handle_)};
    }

    /// The rows [offset, offset+len) as a view with a gather's values and
    /// nulls, keeping the layout and sharing the value buffers (see
    /// dftu_series_slice). Invalid for a list, map, struct or constant column.
    Series slice(std::int64_t offset, std::int64_t len) const noexcept {
        return Series{dftu_series_slice(handle_, offset, len)};
    }

    // Each returns a new owned Series (move-only) so calls chain; binary ops
    // need FLAT numeric columns of equal length and are invalid on a mismatch.
    Series add(const Series& other) const;
    Series sub(const Series& other) const;
    Series mul(const Series& other) const;
    Series div(const Series& other) const;
    /// Python-semantics floor division, remainder and power: Int64 when both
    /// operands are integral, else Float64; a zero divisor or a negative
    /// integer exponent is null. A row that lacks a side after label alignment
    /// is null in every op; pandas follows IEEE `pow` there, so `1 ** NaN` and
    /// `NaN ** 0` are 1.0 in pandas and null here.
    Series floordiv(const Series& other) const;
    Series mod(const Series& other) const;
    Series pow(const Series& other) const;
    /// Nulls filled from the nearest present value before (ffill) or after
    /// (bfill); a leading (trailing) run with none stays null.
    Series ffill() const;
    Series bfill() const;

    /// Reductions over the valid values. A bool column counts true as 1; min
    /// and max of a string column give a STR scalar borrowing the column. A
    /// reduction the engine does not define for the column type (sum of a
    /// string, sum of a timestamp) returns an InvalidArgument error naming the
    /// operation and the type; it never returns 0 for a refusal. A column with
    /// no valid row gives sum 0, and min and max a NULL scalar (no value); the
    /// refusal of a type comes first.
    dstd::Result<Scalar> sum() const;
    dstd::Result<Scalar> min() const;
    dstd::Result<Scalar> max() const;
    Scalar product() const;
    Scalar mode() const;           ///< most frequent non-null value (hash)
    bool all() const;              ///< every non-null Bool bit set
    bool any() const;              ///< at least one non-null Bool bit set
    std::int64_t arg_min() const;  ///< index of min non-null value (-1 if none)
    std::int64_t arg_max() const;  ///< index of max non-null value (-1 if none)
    std::int64_t count() const;    ///< non-null row count
    /// The mean; the error of sum() for a type with no sum. NaN for a column
    /// with no valid row.
    dstd::Result<double> mean() const;
    /// The variance and standard deviation: NaN for a column with no valid row
    /// and, for the sample form, for one valid row.
    double variance(bool sample = true) const;
    double stddev(bool sample = true) const;
    double skewness() const;
    double kurtosis() const;
    /// The pandas sample skewness (NaN below three values) and sample excess
    /// kurtosis (NaN below four); NaN for a column with no spread.
    double skewness_unbiased() const;
    double kurtosis_unbiased() const;
    /// The exact quantile of the valid values, `q` clamped to [0, 1]; NaN for
    /// no valid row or a NaN `q`.
    double quantile(double q) const;
    /// The exact quantile with @p interp choosing the result between ranks.
    double quantile(double q, Interpolation interp) const;
    /// The median; NaN for a column with no valid row.
    double median() const;
    std::int64_t nunique() const;

    /// The distinct values of this column and their counts, as a @ref DataFrame
    /// with columns `value` (this column's type) and `count` (Int64),
    /// most-frequent first. An InvalidArgument error for a type with no
    /// per-row value to count.
    dstd::Result<DataFrame> value_counts() const;

    /// Rank of each row (`method` selects the tie-break), ascending unless
    /// `descending`, as a Float64 column; a null and a float NaN are unranked:
    /// they get a null rank and are not counted.
    Series rank(RankMethod method, bool descending = false,
                bool pct = false) const;
    /// Rolling-window reduction (`op` = Sum/Mean/Min/Max) over `window`
    /// trailing rows (first window-1 null), as Float64.
    Series rolling(RollingOp op, std::int64_t window) const;
    /// Rolling sample variance / std over `window` trailing rows (first
    /// window-1 null), as Float64.
    Series rolling_var(std::int64_t window) const;
    Series rolling_std(std::int64_t window) const;
    /// Rolling median / quantile (`q` in [0,1]) over `window` trailing rows
    /// (first window-1 null), as Float64.
    Series rolling_median(std::int64_t window) const;
    Series rolling_quantile(std::int64_t window, double q) const;
    /// Exponentially weighted mean / sample std, smoothing `alpha` in (0,1], as
    /// Float64 (ewm_std row 0 is null).
    Series ewm_mean(double alpha) const;
    Series ewm_std(double alpha) const;
    /// Bin values by the ascending `breaks` edges (count of breaks <= x) ->
    /// Int32; a null input row gives a null bin, and a null break gives no
    /// column at all (an invalid Series). `flags` is
    /// DFTU_CUT_RIGHT (count the breaks < x: right-closed intervals) and/or
    /// DFTU_CUT_INNER (null outside the interior intervals, bins numbered from
    /// 0). Integer, Bool and temporal values with integer breaks compare
    /// exactly as int64; any float on either side compares as doubles.
    Series cut(const Series& breaks, std::int32_t flags = 0) const;
    /// Bin values by the column's `q`-quantile edges -> Int32 (bins 0..q-1).
    Series qcut(std::int32_t q) const;
    /// Lower-bound insertion index of each `values` element into this (assumed
    /// sorted) column -> Int64.
    Series search_sorted(const Series& values, bool right = false) const;
    /// Linearly interpolate null interior values; leading/trailing nulls stay
    /// null -> Float64.
    Series interpolate() const;
    /// Sum of x[i]*y[i] over the non-null pairs with `other` (SIMD f64).
    Scalar dot(const Series& other) const;

    Series cast(TypeId target) const;
    /// Unary numeric primitive (`op` = Ilog2/BitWidth/Popcount/Clz/Ctz/Mix64)
    /// over a FLAT Int64/Uint64 column, producing an Int64 column; invalid
    /// unless the input is a 64-bit integer column.
    Series prim(PrimOp op) const;
    Series abs() const;
    Series clip(Scalar lo, Scalar hi) const;
    /// Clamp to [lo, hi] with natural C++ values (clip(0, 100)); the bounds
    /// convert to this column's element type.
    template <class Lo, class Hi,
              class = std::enable_if_t<std::is_arithmetic_v<Lo> &&
                                       std::is_arithmetic_v<Hi>>>
    Series clip(Lo lo, Hi hi) const {
        return clip(as_scalar(lo), as_scalar(hi));
    }
    Series round() const;
    Series fillna(Scalar fill) const;
    /// Replace nulls with a natural C++ value (fillna(0), fillna(1.5)); the
    /// fill converts to this column's element type.
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    Series fillna(T fill) const {
        return fillna(as_scalar(fill));
    }
    /// Replace each NaN of a float column with `fill`, converted to the
    /// column's type; a null scalar turns the NaN into a null. A null row stays
    /// null. A column of another type is shared unchanged; a string `fill` on a
    /// float column gives an invalid Series.
    Series fill_nan(Scalar fill) const;
    /// Replace each NaN with a natural C++ value (fill_nan(0.0)).
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    Series fill_nan(T fill) const {
        return fill_nan(as_scalar(fill));
    }
    Series cumsum() const;
    Series cummax() const;
    Series cummin() const;
    Series cum_prod() const;
    Series cum_count() const;
    Series ceil() const;
    Series floor() const;
    Series trunc() const;
    Series sign() const;
    Series negate() const;
    Series diff() const;
    Series pct_change() const;
    Series sqrt() const;
    Series exp() const;
    Series log() const;
    /// log(1 + x) and exp(x) - 1, exact for a tiny x.
    Series log1p() const;
    Series expm1() const;
    /// Elementwise math: a Float32 column stays Float32, any other numeric
    /// column gives Float64; a value outside the domain is NaN. cot is
    /// 1 / tan.
    Series sin() const;
    Series cos() const;
    Series tan() const;
    Series cot() const;
    Series arcsin() const;
    Series arccos() const;
    Series arctan() const;
    Series sinh() const;
    Series cosh() const;
    Series tanh() const;
    Series arcsinh() const;
    Series arccosh() const;
    Series arctanh() const;
    Series cbrt() const;
    Series log10() const;
    Series erf() const;
    Series erfc() const;
    /// Bit counts of an integer or Bool column as a Uint32 column, within the
    /// column's own width; a signed value is read as its two's complement
    /// bits. Another column type gives an invalid Series.
    Series bitwise_count_ones() const;
    Series bitwise_count_zeros() const;
    Series bitwise_leading_ones() const;
    Series bitwise_leading_zeros() const;
    Series bitwise_trailing_ones() const;
    Series bitwise_trailing_zeros() const;
    /// The bitwise AND, OR and XOR of the non-null values of an integer or
    /// Bool column: a signed integer or a Bool gives an I64 Scalar, an unsigned
    /// integer a U64 one, a column with no non-null value a NULL one, another
    /// column type an ERR one.
    Scalar bitwise_and() const;
    Scalar bitwise_or() const;
    Scalar bitwise_xor() const;
    /// The distinct values, a null kept once; first-occurrence order when
    /// `maintain_order`, otherwise ascending with the null first.
    Series unique(bool maintain_order = false) const;
    Series dictionary_encode() const;

    Series null_mask() const;   ///< Bool mask: row is null.
    Series valid_mask() const;  ///< Bool mask: row is present.
    /// Bool mask: row is null or, in a float column, NaN (pandas isna).
    Series na_mask() const;
    /// Bool mask: row is present and not NaN (pandas notna).
    Series notna_mask() const;
    Series is_nan() const;         ///< Bool mask: value is NaN (floats).
    Series is_finite() const;      ///< Bool mask: value is finite.
    Series is_infinite() const;    ///< Bool mask: value is +/-Inf (floats).
    Series is_unique() const;      ///< Bool mask: value occurs exactly once.
    Series is_duplicated() const;  ///< Bool mask: value is repeated.
    /// Bool mask of the repeats: `keep` 0 leaves the first row of each value
    /// unmarked, 1 the last; another value marks every row of a repeated value.
    Series duplicated(std::int32_t keep = 0) const;
    /// Whether the column is sorted ascending (or descending) with every null
    /// after the values, as @ref argsort orders it; single scan.
    bool is_sorted(bool descending = false) const;
    Series drop_nulls() const;  ///< Selection over the non-null rows.
    /// The rows that are not NaN, nulls kept; a column that is not a float is
    /// shared unchanged. An invalid Series when a float column cannot be read.
    Series drop_nans() const;
    /// Bool mask, true where the value is present in `values`.
    Series is_in(const Series& values) const;
    /// Rows sorted ascending (or descending): take(argsort(v, descending,
    /// nulls_last)). Nulls go last, or first when `nulls_last` is false.
    Series sort(bool descending = false, bool nulls_last = true) const;
    Series head(std::int64_t n) const;  ///< First n rows (n clamped to length).
    Series tail(std::int64_t n) const;  ///< Last n rows (n clamped to length).
    Series reverse() const;             ///< Rows in reverse order.
    /// Shift rows by n (positive = lag, negative = lead); vacated rows null.
    Series shift(std::int64_t n) const;
    Series top_k(std::int64_t k) const;     ///< The k largest rows, best-first.
    Series bottom_k(std::int64_t k) const;  ///< The k smallest rows.
    /// A deterministic sample of n distinct rows (mix64(index, seed) hash).
    Series sample(std::int64_t n, std::uint64_t seed = 0) const;

    Series argsort(bool descending = false, bool nulls_last = true) const;
    Series take(dstd::Span<const std::int64_t> indices) const;
    Series materialize() const;
    /// Keep the rows where the bit-packed Bool `mask` (same length) is true.
    Series filter(const Series& mask) const;
    /// Per row, this column's value where `mask` is true, else `other`'s
    /// (`mask ? this : other`); both must share a type and length. Columns
    /// that carry row labels must carry the same ones (@ref same_labels): an
    /// invalid column otherwise. The result takes the labels of this column,
    /// else of @p other, else of @p mask.
    Series where(const Series& mask, const Series& other) const;

    Series str_eq(dstd::StringView rhs) const;
    Series str_contains(dstd::StringView needle) const;
    Series str_starts_with(dstd::StringView prefix) const;
    Series str_ends_with(dstd::StringView suffix) const;
    /// Bool mask: the whole string matches the duql regex `pattern`.
    Series str_matches(dstd::StringView pattern) const;
    /// Bool mask: the duql regex `pattern` matches anywhere in the
    /// string.
    Series str_search(dstd::StringView pattern) const;
    /// Bool mask: the string matches the SQL LIKE / glob `pattern` (`%` any
    /// run,
    /// `_` one char, `\` escapes a literal `%`/`_`/`\`).
    Series str_like(dstd::StringView pattern) const;
    Series str_len_bytes() const;  ///< Int64 per-row byte length.
    Series str_len_chars() const;  ///< Int64 per-row UTF-8 codepoint count.
    /// Int64 byte index of the first `needle`, or -1.
    Series str_find(dstd::StringView needle) const;
    Series fnv1a() const;  ///< UInt64 FNV-1a 64 of each row's bytes.
    /// UInt64 parse of each row as dftracer's 16-hex-digit hash form;
    /// a row not in that form is null.
    Series hex64_parse() const;
    /// String format of each UInt64/Int64 row as dftracer's 16-lowercase-hex-
    /// digit hash form; the inverse of hex64_parse.
    Series hex64_format() const;
    Series to_lowercase() const;  ///< ASCII case fold (non-ASCII unchanged).
    Series to_uppercase() const;  ///< ASCII case fold (non-ASCII unchanged).
    Series str_strip() const;     ///< Trim ASCII whitespace both ends.
    Series str_lstrip() const;    ///< Trim ASCII whitespace left.
    Series str_rstrip() const;    ///< Trim ASCII whitespace right.
    /// Replace the first / all literal `pat` with `repl` (empty `pat` = no-op).
    Series str_replace(dstd::StringView pat, dstd::StringView repl) const;
    Series str_replace_all(dstd::StringView pat, dstd::StringView repl) const;
    /// Substring by byte offset (negative `start` from end; negative `length`
    /// to end; clamped).
    Series str_slice(std::int64_t start, std::int64_t length) const;
    /// Pad to `width` bytes with `fill` on the left / right.
    Series str_pad_start(std::int64_t width, char fill = ' ') const;
    Series str_pad_end(std::int64_t width, char fill = ' ') const;
    /// Left-pad with '0' to `width` after an optional sign (Python zfill).
    Series str_zfill(std::int64_t width) const;
    /// Split each row on literal `sep` -> List<String> column.
    Series str_split(dstd::StringView sep) const;
    /// The capture `group` of the first regex `pattern` match per row; null
    /// where there is no match.
    Series str_extract(dstd::StringView pattern, std::int64_t group = 1) const;
    /// Replace every non-overlapping regex `pattern` match per row with `to`
    /// (`$n`, `${n}`, `${name}` insert a group, `$$` a dollar sign). Null rows
    /// stay null. An invalid pattern or `to` yields an invalid Series.
    Series str_regex_replace(dstd::StringView pattern,
                             dstd::StringView to) const;
    /// Per-row element count of a List column (Int64).
    Series list_size() const;
    /// The element at `index` (negative from the end) of each row's list,
    /// null where the list is too short.
    Series list_get(std::int64_t index) const;
    /// Each row's List<String> joined with `sep`.
    Series list_join(dstd::StringView sep) const;
    /// The pandas .str batch over ASCII (dataframe/c_api.h documents each):
    /// case forms by dftu_str_case, character classes by dftu_str_class.
    Series str_case(std::int32_t op) const;
    Series str_is(std::int32_t cls) const;
    Series str_count(dstd::StringView pat) const;
    Series str_rfind(dstd::StringView needle) const;
    Series str_remove_prefix(dstd::StringView prefix) const;
    Series str_remove_suffix(dstd::StringView suffix) const;
    Series str_repeat(std::int64_t n) const;
    Series str_center(std::int64_t width, char fill = ' ') const;
    /// Row-wise concatenation; two columns that carry row labels must carry
    /// the same ones (@ref same_labels), an invalid column otherwise.
    Series str_cat(const Series& other) const;
    Series str_findall(dstd::StringView pattern) const;
    Series str_partition(dstd::StringView sep, bool from_right = false) const;
    /// Unicode normalization (dftu_norm_form), the codepoint table of
    /// `from` -> `to` (see dftu_series_str_translate) and textwrap by
    /// `width` codepoints with dftu_wrap_flag bits; a row that is not valid
    /// UTF-8 is null.
    Series str_normalize(std::int32_t form) const;
    Series str_translate(const Series& from, const Series& to) const;
    Series str_wrap(std::int64_t width, std::int32_t flags) const;
    /// A calendar part (dftu_dt_part) as Int64, `unit` (dftu_time_unit) for
    /// an Int64 input; each instant rounded to a bucket of `every` units by
    /// dftu_dt_round_mode.
    Series dt_part(std::int32_t part,
                   std::int32_t unit = DFTU_TIME_UNIT_MICRO) const;
    Series dt_round(std::int64_t every, std::int32_t mode) const;
    /// Each instant as UTC text by the strftime-style `fmt` (%Y %y %m %d %H %I
    /// %M %S %f %j %a %A %b %B %p %F %T %s %z %Z %%); `unit` is the unit of an
    /// Int64 input. Invalid (not valid()) for an unknown directive, a
    /// trailing '%' or an input that is not a time or Int64 column.
    Series dt_format(dstd::StringView fmt,
                     TimeUnit unit = TimeUnit::Micro) const;
    /// dftu_series_dt_as_unit: a Timestamp or Duration column in `unit`,
    /// the instant kept, a coarser unit rounded by `mode`
    /// (dftu_dt_unit_rounding). Invalid (not valid()) for another type or
    /// when a value overflows Int64 in the finer unit.
    Series dt_as_unit(TimeUnit unit,
                      std::int32_t mode = DFTU_DT_UNIT_FLOOR) const;
    /// dftu_series_dt_date: each Timestamp (its wall day in the column's
    /// zone, else the UTC day) or Date64 row as Date32.
    Series dt_date() const;
    /// dftu_series_dt_time: each Timestamp row's time of day (wall time in the
    /// column's zone, else UTC), in the column's own unit (Time32 for s and
    /// ms, Time64 for us and ns).
    Series dt_time() const;
    /// dftu_series_with_timezone: this Timestamp column under zone `tz`
    /// (empty for naive), buffers shared. Invalid (not valid()) for a zone
    /// that is not in the time zone database.
    Series with_timezone(dstd::StringView tz) const;
    /// dftu_series_tz_localize: this column's wall times in zone `tz` as UTC
    /// instants (dftu_tz_ambiguous, dftu_tz_nonexistent; `shift` in ticks).
    /// Invalid (not valid()) for a refused wall time or an unknown zone.
    Series tz_localize(dstd::StringView tz, std::int32_t ambiguous,
                       std::int32_t nonexistent, std::int64_t shift = 0) const;

    Series logical_and(const Series& other) const;
    Series logical_or(const Series& other) const;
    Series logical_not() const;

    /// Compare against a scalar, returning a bit-packed Bool mask; the scalar
    /// is converted to this column's element type (exact for i64).
    template <class T>
    Series gt(T v) const {
        return compare_to(DFTU_CMP_GT, v);
    }
    template <class T>
    Series ge(T v) const {
        return compare_to(DFTU_CMP_GE, v);
    }
    template <class T>
    Series lt(T v) const {
        return compare_to(DFTU_CMP_LT, v);
    }
    template <class T>
    Series le(T v) const {
        return compare_to(DFTU_CMP_LE, v);
    }
    template <class T>
    Series eq(T v) const {
        return compare_to(DFTU_CMP_EQ, v);
    }
    template <class T>
    Series ne(T v) const {
        return compare_to(DFTU_CMP_NE, v);
    }

    /// Bit-packed Bool mask, true where lo <= value <= hi (inclusive); the
    /// bounds convert to this column's element type.
    template <class T>
    Series is_between(T lo, T hi) const {
        return derived(
            dftu_series_is_between(handle_, as_scalar(lo), as_scalar(hi)));
    }

    // Comparison operators return a mask (columnar convention), so equality
    // stays as eq()/ne() rather than operator==.
    Series operator+(const Series& o) const { return add(o); }
    Series operator-(const Series& o) const { return sub(o); }
    Series operator*(const Series& o) const { return mul(o); }
    Series operator/(const Series& o) const { return div(o); }

    // Scalar arithmetic (elementwise against a constant), mirroring the
    // Series-Series operators; the scalar converts to this column's element
    // type. A Series right operand takes the non-template overloads above.
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    Series operator+(T v) const {
        return derived(dftu_series_add_scalar(handle_, as_scalar(v)));
    }
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    Series operator-(T v) const {
        return derived(dftu_series_sub_scalar(handle_, as_scalar(v)));
    }
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    Series operator*(T v) const {
        return derived(dftu_series_mul_scalar(handle_, as_scalar(v)));
    }
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    Series operator/(T v) const {
        return derived(dftu_series_div_scalar(handle_, as_scalar(v)));
    }
    Series operator&(const Series& o) const { return logical_and(o); }
    Series operator|(const Series& o) const { return logical_or(o); }
    Series operator~() const { return logical_not(); }
    template <class T>
    Series operator<(T v) const {
        return lt(v);
    }
    template <class T>
    Series operator<=(T v) const {
        return le(v);
    }
    template <class T>
    Series operator>(T v) const {
        return gt(v);
    }
    template <class T>
    Series operator>=(T v) const {
        return ge(v);
    }

    /// Compare against a scalar with a runtime op code, producing a Bool mask.
    /// For a planner holding op and rhs as values.
    Series compare(CmpOp op, Scalar rhs) const {
        return derived(
            dftu_series_compare(handle_, static_cast<dftu_cmp_op>(op), rhs));
    }

   private:
    static Series structs_from(dstd::Span<const char* const> names,
                               dstd::Vector<Series> columns);

    template <class T>
    static dftu_scalar as_scalar(T v) {
        return to_scalar(v);
    }

    template <class T>
    Series compare_to(std::int32_t op, T v) const {
        return derived(dftu_series_compare(
            handle_, static_cast<dftu_cmp_op>(op), as_scalar(v)));
    }

    /// Wraps @p result, a column derived from this one, with this column's
    /// name.
    Series derived(dftu_series* result) const;
    /// Same for a binary op: the name is kept only when @p other has the same.
    Series derived(dftu_series* result, const Series& other) const;
    /// A binary op over this column and @p other, aligned on their row labels
    /// first (see @ref align_labels); an invalid column when they cannot align.
    template <class Op>
    Series paired(const Series& other, Op op) const;

    dftu_series* handle_ = nullptr;
};

/// Two columns aligned on their row labels, each carrying the result labels.
struct AlignedSeries {
    Series left;
    Series right;
};

/// Aligns `a` and `b` for a binary op as pandas does. Two positional columns
/// pass through unchanged (their lengths are the op's to check). A positional
/// column counts as the labels 0 .. length-1 once the other has labels. Labels
/// that are the same by value, level for level, and in the same order leave
/// the rows in place under those labels. Otherwise the result is the outer
/// join of the labels, sorted by them, with a null where a side has no row
/// (an integer column that gains a null becomes Float64 first, a Bool one
/// takes false, as the pandas logical ops); a duplicated label matches every
/// row that has it. A null label equals another null label (the pandas index
/// treats NaN as one label) and sorts last. Numeric labels of different types
/// meet in the numpy common type (Int32 with Int64 in Int64, an integer with
/// a float in Float64); a string label never meets a number. The result labels
/// take `a`'s level names (the default `__index_level_<i>__` for a positional
/// `a`), as pandas. An InvalidArgument error when the labels have a different
/// number of levels or a level whose two types have no common type, or when the
/// engine's join refuses the labels.
dstd::Result<AlignedSeries> align_labels(const Series& a, const Series& b);

/// Aligns `a` and `b` on their labels as pandas `Series.align(join=how)`: the
/// result labels are those of the outer join (sorted, nulls last), the inner
/// join (the order of `a`), `a`'s own (Left) or `b`'s own (Right), a repeated
/// label meeting every row that has it (the rows of `a` outermost). Labels that
/// are the same, level for level, leave the rows in place. A column that
/// misses a row has a null there (an integer column becomes Float64). A
/// positional column counts as 0 .. length-1 even when the other has none, so
/// two positional columns of different lengths align too. The errors of
/// @ref align_labels, and an InvalidArgument error for a `how` that is not
/// Inner, Left, Right or Outer.
dstd::Result<AlignedSeries> align_labels(const Series& a, const Series& b,
                                         JoinHow how);

/// Whether `a` and `b` carry the same labels as @ref align_labels counts them
/// (a positional column is 0 .. length-1), so a comparison needs no alignment.
bool same_labels(const Series& a, const Series& b);

// Reflected scalar arithmetic: `v + s`, `v * s` (commutative) and `v - s`.
// Reflected division `v / s` is intentionally absent (no reciprocal kernel).
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
inline Series operator+(T v, const Series& s) {
    return s + v;
}
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
inline Series operator*(T v, const Series& s) {
    return s * v;
}
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
inline Series operator-(T v, const Series& s) {
    return s * (-1) + v;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_SERIES_H
