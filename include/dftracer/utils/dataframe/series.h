#ifndef DFTRACER_UTILS_DATAFRAME_SERIES_H
#define DFTRACER_UTILS_DATAFRAME_SERIES_H

#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dataframe/types.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
struct ArrowSchema;
struct ArrowArray;
#endif

namespace dftracer::utils::dataframe {

struct DataFrame;
#ifdef DFTRACER_UTILS_ENABLE_ARROW
class OwnedArrow;
#endif

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
    /// copied if non-null (usually null). Returns an invalid Series for a
    /// variable-width type (String/Binary), where borrowing a single flat
    /// buffer is not meaningful. Mirrors dftu_series_new_flat_borrowed.
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
    /// before an invalid Series is returned. `validity` is copied if non-null.
    template <class Owner, class = std::enable_if_t<!std::is_convertible_v<
                               Owner&&, const std::uint8_t*>>>
    static Series from_borrowed(TypeId type, const void* data, std::int64_t n,
                                Owner&& owner,
                                const std::uint8_t* validity = nullptr) {
        using Held = std::decay_t<Owner>;
        auto* held = new Held(std::forward<Owner>(owner));
        // A rejected borrow (variable-width) runs release() itself, so `held`
        // is freed there; never double-free it here.
        return Series{dftu_series_new_flat_borrowed(
            static_cast<dftu_dtype>(type), data, n, validity,
            [](void* ctx) { delete static_cast<Held*>(ctx); }, held)};
    }

    /// Build a FLAT String column from `values` (offsets + concatenated bytes).
    static Series strings(const std::vector<std::string>& values) {
        std::vector<std::int32_t> offsets(values.size() + 1, 0);
        std::string data;
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
    static Series strings(std::span<const std::string_view> values) {
        std::vector<std::int32_t> offsets(values.size() + 1, 0);
        std::size_t total = 0;
        for (const auto& v : values) total += v.size();
        std::string data;
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
    static Series strings(std::span<const std::string_view> values,
                          const std::uint8_t* validity) {
        std::vector<std::int32_t> offsets(values.size() + 1, 0);
        std::size_t total = 0;
        for (const auto& v : values) total += v.size();
        std::string data;
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
        std::vector<std::uint8_t> validity(
            static_cast<std::size_t>((n + 7) / 8), 0);  // every row null
        if (type == TypeId::String || type == TypeId::Binary) {
            std::vector<std::int32_t> offsets(static_cast<std::size_t>(n) + 1,
                                              0);
            return Series{dftu_series_new_string(static_cast<dftu_dtype>(type),
                                                 offsets.data(), "", n,
                                                 validity.data())};
        }
        std::vector<std::uint8_t> data(buffer_bytes(type, n), 0);
        return Series{dftu_series_new_flat(static_cast<dftu_dtype>(type),
                                           data.data(), n, validity.data())};
    }

    /// Build a STRUCT column from field `names` and `columns` (each the same
    /// length, aligned to names); takes ownership of the field columns. The
    /// columns are move-only, so pass them in a vector built by move.
    static Series structs(std::vector<std::string> names,
                          std::vector<Series> columns) {
        std::vector<dftu_series*> handles;
        std::vector<const char*> cnames;
        handles.reserve(columns.size());
        cnames.reserve(names.size());
        for (Series& c : columns) handles.push_back(c.release());
        for (const std::string& s : names) cnames.push_back(s.c_str());
        return Series{
            dftu_series_new_struct(handles.data(), cnames.data(),
                                   static_cast<std::int32_t>(handles.size()))};
    }

    /// Build a LIST column: `offsets` (n+1 entries) index into `values`; takes
    /// ownership of `values`.
    static Series list(const std::vector<std::int32_t>& offsets,
                       Series values) {
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
    std::span<const T> values() const noexcept {
        const T* p = data<T>();
        return p != nullptr
                   ? std::span<const T>(p, static_cast<std::size_t>(length()))
                   : std::span<const T>();
    }

    /// Value of a String/Binary/LargeString/LargeBinary column at `i`, valid
    /// while this column lives.
    ///
    /// FLAT only: a DICTIONARY or SELECTION column (what `filter`, `take` and
    /// the sort/topk kernels return, zero-copy over a base) carries no value
    /// buffer of its own, so there is nothing to read at `i`. Those return an
    /// empty view; call `materialize()` first to read them. Empty is therefore
    /// ambiguous with a genuine empty string - use `is_flat()` when the
    /// difference matters.
    std::string_view string_at(std::int64_t i) const noexcept {
        const char* d = static_cast<const char*>(dftu_series_data(handle_));
        // dftu_series_data returns NULL for a non-FLAT column; reading through
        // it produced a segfault rather than a diagnosable result.
        if (d == nullptr) return {};
        if (is_wide_offset_type(type())) {
            const std::int64_t* off = dftu_series_offsets64(handle_);
            if (off == nullptr) return {};
            return std::string_view(
                d + off[i], static_cast<std::size_t>(off[i + 1] - off[i]));
        }
        const std::int32_t* off = dftu_series_offsets(handle_);
        if (off == nullptr) return {};
        return std::string_view(d + off[i],
                                static_cast<std::size_t>(off[i + 1] - off[i]));
    }

    /// Whether the values live in this column's own buffers, so `data()`,
    /// `offsets()` and `string_at()` can read them. False for CONSTANT,
    /// DICTIONARY and SELECTION, which `materialize()` converts.
    bool is_flat() const noexcept { return encoding() == Encoding::Flat; }

    /// int32 offset buffer (length+1 entries) of a String/Binary or List
    /// column, or null for fixed-width types and for a LargeString/
    /// LargeBinary/LargeList column (see `offsets64()`).
    const std::int32_t* offsets() const noexcept {
        return dftu_series_offsets(handle_);
    }

    /// int32 offset buffer as a span of `length()+1` entries; empty for
    /// fixed-width types and for a Large* column.
    std::span<const std::int32_t> offsets_span() const noexcept {
        const std::int32_t* off = offsets();
        return off != nullptr ? std::span<const std::int32_t>(
                                    off, static_cast<std::size_t>(length()) + 1)
                              : std::span<const std::int32_t>();
    }

    /// int64 offset buffer (length+1 entries) of a LargeString/LargeBinary/
    /// LargeList column, or null otherwise.
    const std::int64_t* offsets64() const noexcept {
        return dftu_series_offsets64(handle_);
    }

    /// int64 offset buffer as a span of `length()+1` entries; empty unless
    /// this is a Large* column.
    std::span<const std::int64_t> offsets64_span() const noexcept {
        const std::int64_t* off = offsets64();
        return off != nullptr ? std::span<const std::int64_t>(
                                    off, static_cast<std::size_t>(length()) + 1)
                              : std::span<const std::int64_t>();
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
    std::string field_name(std::int64_t i) const {
        const char* s =
            dftu_series_field_name(handle_, static_cast<std::int32_t>(i));
        return s ? std::string(s) : std::string();
    }

    /// The full nested DataType, walking `num_children()`/`child()` and each
    /// Struct field's name. A List's single child becomes its (unnamed)
    /// element Field; a Struct's children become named Fields in order.
    DataType data_type() const;

    /// A new owned column sharing this column's buffers zero-copy (refcount
    /// bump, no data copy).
    Series share() const noexcept { return Series{dftu_series_share(handle_)}; }

    /// A view of the FLAT rows [offset, offset+len); invalid unless FLAT and
    /// fixed-width. Zero-copy for the values; a null bitmap is shared when the
    /// offset is byte-aligned and re-packed to bit 0 otherwise.
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
    /// integer exponent is null.
    Series floordiv(const Series& other) const;
    Series mod(const Series& other) const;
    Series pow(const Series& other) const;
    /// Nulls filled from the nearest present value before (ffill) or after
    /// (bfill); a leading (trailing) run with none stays null.
    Series ffill() const;
    Series bfill() const;

    Scalar sum() const;
    Scalar min() const;
    Scalar max() const;
    Scalar product() const;
    Scalar mode() const;           ///< most frequent non-null value (hash)
    bool all() const;              ///< every non-null Bool bit set
    bool any() const;              ///< at least one non-null Bool bit set
    std::int64_t arg_min() const;  ///< index of min non-null value (-1 if none)
    std::int64_t arg_max() const;  ///< index of max non-null value (-1 if none)
    std::int64_t count() const;    ///< non-null row count
    double mean() const;
    double variance(bool sample = true) const;
    double stddev(bool sample = true) const;
    double skewness() const;
    double kurtosis() const;
    double quantile(double q) const;
    double median() const;
    std::int64_t nunique() const;

    /// The distinct values of this column and their counts, as a DataFrame with
    /// columns `value` (this column's type) and `count` (Int64), most-frequent
    /// first.
    DataFrame value_counts() const;

    /// Rank of each row (`method` selects the tie-break), ascending unless
    /// `descending`, as a Float64 column; nulls rank last with a NaN rank.
    Series rank(RankMethod method, bool descending = false) const;
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
    /// Int32; a null input row yields a null bin.
    Series cut(const Series& breaks) const;
    /// Bin values by the column's `q`-quantile edges -> Int32 (bins 0..q-1).
    Series qcut(std::int32_t q) const;
    /// Lower-bound insertion index of each `values` element into this (assumed
    /// sorted) column -> Int64.
    Series search_sorted(const Series& values) const;
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
        return clip(to_scalar_(lo), to_scalar_(hi));
    }
    Series round() const;
    Series fillna(Scalar fill) const;
    /// Replace nulls with a natural C++ value (fillna(0), fillna(1.5)); the
    /// fill converts to this column's element type.
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    Series fillna(T fill) const {
        return fillna(to_scalar_(fill));
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
    Series unique() const;
    Series dictionary_encode() const;

    Series null_mask() const;      ///< Bool mask: row is null.
    Series valid_mask() const;     ///< Bool mask: row is present.
    Series is_nan() const;         ///< Bool mask: value is NaN (floats).
    Series is_finite() const;      ///< Bool mask: value is finite.
    Series is_infinite() const;    ///< Bool mask: value is +/-Inf (floats).
    Series is_unique() const;      ///< Bool mask: value occurs exactly once.
    Series is_duplicated() const;  ///< Bool mask: value is repeated.
    /// Whether the column is sorted ascending (or descending); single scan.
    bool is_sorted(bool descending = false) const;
    Series drop_nulls() const;  ///< Selection over the non-null rows.
    /// Bool mask, true where the value is present in `values`.
    Series is_in(const Series& values) const;
    /// Rows sorted ascending (or descending): take(argsort(v, descending)).
    Series sort(bool descending = false) const;
    Series head(std::int64_t n) const;  ///< First n rows (n clamped to length).
    Series tail(std::int64_t n) const;  ///< Last n rows (n clamped to length).
    Series reverse() const;             ///< Rows in reverse order.
    /// Shift rows by n (positive = lag, negative = lead); vacated rows null.
    Series shift(std::int64_t n) const;
    Series top_k(std::int64_t k) const;     ///< The k largest rows, best-first.
    Series bottom_k(std::int64_t k) const;  ///< The k smallest rows.
    /// A deterministic sample of n distinct rows (mix64(index, seed) hash).
    Series sample(std::int64_t n, std::uint64_t seed = 0) const;

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    /// Export this column through the Arrow C Data Interface (zero copy: the
    /// exported buffers alias this column's and are kept alive by the returned
    /// owner's release callbacks). Include
    /// <dftracer/utils/dataframe/arrow.h> for the OwnedArrow definition.
    OwnedArrow to_arrow() const;

    /// Import an Arrow array (viewed through `schema`) as a Series, wrapping
    /// its buffers zero copy. ADOPTS `array`: ownership of its buffers moves
    /// into the returned Series (the array's release runs when the Series is
    /// destroyed) and the passed array is marked released, so the caller must
    /// not release it again. `schema` is only read. An invalid Series on an
    /// unsupported type.
    static Series from_arrow(const ArrowSchema* schema,
                             const ArrowArray* array);
#endif

    Series argsort(bool descending = false) const;
    Series take(const std::vector<std::int64_t>& indices) const;
    Series take(std::span<const std::int64_t> indices) const;
    Series materialize() const;
    /// Keep the rows where the bit-packed Bool `mask` (same length) is true.
    Series filter(const Series& mask) const;
    /// Per row, this column's value where `mask` is true, else `other`'s
    /// (`mask ? this : other`); both must share a type and length.
    Series where(const Series& mask, const Series& other) const;

    Series str_eq(std::string_view rhs) const;
    Series str_contains(std::string_view needle) const;
    Series str_starts_with(std::string_view prefix) const;
    Series str_ends_with(std::string_view suffix) const;
    /// Bool mask: the whole string matches the ECMAScript regex `pattern`.
    Series str_matches(std::string_view pattern) const;
    /// Bool mask: the ECMAScript regex `pattern` matches anywhere in the
    /// string.
    Series str_search(std::string_view pattern) const;
    /// Bool mask: the string matches the SQL LIKE / glob `pattern` (`%` any
    /// run,
    /// `_` one char, `\` escapes a literal `%`/`_`/`\`).
    Series str_like(std::string_view pattern) const;
    Series str_len_bytes() const;  ///< Int64 per-row byte length.
    Series str_len_chars() const;  ///< Int64 per-row UTF-8 codepoint count.
    /// Int64 byte index of the first `needle`, or -1.
    Series str_find(std::string_view needle) const;
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
    Series str_replace(std::string_view pat, std::string_view repl) const;
    Series str_replace_all(std::string_view pat, std::string_view repl) const;
    /// Substring by byte offset (negative `start` from end; negative `length`
    /// to end; clamped).
    Series str_slice(std::int64_t start, std::int64_t length) const;
    /// Pad to `width` bytes with `fill` on the left / right.
    Series str_pad_start(std::int64_t width, char fill = ' ') const;
    Series str_pad_end(std::int64_t width, char fill = ' ') const;
    /// Left-pad with '0' to `width` after an optional sign (Python zfill).
    Series str_zfill(std::int64_t width) const;
    /// Split each row on literal `sep` -> List<String> column.
    Series str_split(std::string_view sep) const;
    /// The capture `group` of the first regex `pattern` match per row; null
    /// where there is no match.
    Series str_extract(std::string_view pattern, std::int64_t group = 1) const;
    /// Per-row element count of a List column (Int64).
    Series list_len() const;
    /// The element at `index` (negative from the end) of each row's list,
    /// null where the list is too short.
    Series list_get(std::int64_t index) const;
    /// Each row's List<String> joined with `sep`.
    Series list_join(std::string_view sep) const;
    /// The pandas .str batch over ASCII (dataframe/abi.h documents each):
    /// case forms by dftu_str_case, character classes by dftu_str_class.
    Series str_case(std::int32_t op) const;
    Series str_is(std::int32_t cls) const;
    Series str_count(std::string_view pat) const;
    Series str_rfind(std::string_view needle) const;
    Series str_remove_prefix(std::string_view prefix) const;
    Series str_remove_suffix(std::string_view suffix) const;
    Series str_repeat(std::int64_t n) const;
    Series str_center(std::int64_t width, char fill = ' ') const;
    Series str_cat(const Series& other) const;
    Series str_findall(std::string_view pattern) const;
    Series str_partition(std::string_view sep, bool from_right = false) const;
    /// A calendar part (dftu_dt_part) as Int64, `unit` (dftu_time_unit) for
    /// an Int64 input; each instant rounded to a bucket of `every` units by
    /// dftu_dt_round_mode.
    Series dt_part(std::int32_t part,
                   std::int32_t unit = DFTU_TIME_UNIT_MICRO) const;
    Series dt_round(std::int64_t every, std::int32_t mode) const;
    /// dftu_series_with_timezone: this Timestamp column under zone `tz`
    /// (empty for naive), buffers shared.
    Series with_timezone(std::string_view tz) const;

    Series logical_and(const Series& other) const;
    Series logical_or(const Series& other) const;
    Series logical_not() const;

    /// Compare against a scalar, returning a bit-packed Bool mask; the scalar
    /// is converted to this column's element type (exact for i64).
    template <class T>
    Series gt(T v) const {
        return compare_(DFTU_CMP_GT, v);
    }
    template <class T>
    Series ge(T v) const {
        return compare_(DFTU_CMP_GE, v);
    }
    template <class T>
    Series lt(T v) const {
        return compare_(DFTU_CMP_LT, v);
    }
    template <class T>
    Series le(T v) const {
        return compare_(DFTU_CMP_LE, v);
    }
    template <class T>
    Series eq(T v) const {
        return compare_(DFTU_CMP_EQ, v);
    }
    template <class T>
    Series ne(T v) const {
        return compare_(DFTU_CMP_NE, v);
    }

    /// Bit-packed Bool mask, true where lo <= value <= hi (inclusive); the
    /// bounds convert to this column's element type.
    template <class T>
    Series is_between(T lo, T hi) const {
        return Series{
            dftu_series_is_between(handle_, to_scalar_(lo), to_scalar_(hi))};
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
        return Series{dftu_series_add_scalar(handle_, to_scalar_(v))};
    }
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    Series operator-(T v) const {
        return Series{dftu_series_sub_scalar(handle_, to_scalar_(v))};
    }
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    Series operator*(T v) const {
        return Series{dftu_series_mul_scalar(handle_, to_scalar_(v))};
    }
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    Series operator/(T v) const {
        return Series{dftu_series_div_scalar(handle_, to_scalar_(v))};
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
        return Series{
            dftu_series_compare(handle_, static_cast<dftu_cmp_op>(op), rhs)};
    }

   private:
    template <class T>
    static dftu_scalar to_scalar_(T v) {
        return to_scalar(v);
    }

    template <class T>
    Series compare_(std::int32_t op, T v) const {
        return Series{dftu_series_compare(handle_, static_cast<dftu_cmp_op>(op),
                                          to_scalar_(v))};
    }

    dftu_series* handle_ = nullptr;
};

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
