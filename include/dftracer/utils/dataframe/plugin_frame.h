// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_PLUGIN_FRAME_H
#define DFTRACER_UTILS_DATAFRAME_PLUGIN_FRAME_H

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

/// A frame the host owns, a column a plugin builds, the builder that puts
/// columns into a frame, and the read views over both. A source, a node, a
/// file format and a UDF all give the host a frame or a column: @ref Column
/// takes the plugin's buffers by move and hands them to the engine without a
/// copy, @ref FrameBuilder names columns and builds the frame, and
/// @ref ColumnView and @ref FrameView read a frame the host already holds
/// without a copy and without an allocation. Every call that can fail reports
/// it as a @ref dstd::Result, and a column made on the way to a failed build is
/// freed by its owner.
namespace dftracer::utils::dataframe {

class ColumnView;
class FrameView;

/// A native columnar result the plugin surface hands back to the engine, freed
/// through the dataframe C ABI. Lets an engine-backed result (the aggregation
/// accumulator) cross as our own @ref dftracer::utils::dataframe::DataFrame
/// "DataFrame" with no Arrow round-trip. The one frame owner type of the
/// plugin surface: the frame that a named result, a source, a node or a format
/// hands back.
class OwnedDataFrame {
   public:
    OwnedDataFrame() noexcept = default;
    explicit OwnedDataFrame(dftu_dataframe* frame) noexcept : frame_(frame) {}
    OwnedDataFrame(const OwnedDataFrame&) = delete;
    OwnedDataFrame& operator=(const OwnedDataFrame&) = delete;
    OwnedDataFrame(OwnedDataFrame&& other) noexcept : frame_(other.release()) {}
    OwnedDataFrame& operator=(OwnedDataFrame&& other) noexcept {
        if (this != &other) {
            reset();
            frame_ = other.release();
        }
        return *this;
    }
    ~OwnedDataFrame() { reset(); }

    /// The frame, or null when there is none. It stays owned by this object.
    dftu_dataframe* get() const noexcept { return frame_; }
    explicit operator bool() const noexcept { return frame_ != nullptr; }
    bool is_null() const noexcept { return frame_ == nullptr; }

    /// The frame without a copy. A view over it stays valid only while this
    /// object, or whatever it is released to, keeps the frame alive.
    FrameView view() const noexcept;

    /// Give up ownership; the caller frees the frame (or hands it to something
    /// that will), and the destructor no longer does.
    dftu_dataframe* release() noexcept {
        dftu_dataframe* const out = frame_;
        frame_ = nullptr;
        return out;
    }
    void reset() noexcept {
        if (frame_ != nullptr) {
            dftu_dataframe_free(frame_);
        }
        frame_ = nullptr;
    }

   private:
    dftu_dataframe* frame_ = nullptr;
};

/// One column the plugin made, with the release callback its buffers need.
/// Move only; the last copy of the handle frees the buffer exactly once.
///
/// A named constructor takes its buffers by move and borrows them, so the
/// engine reads the plugin's memory and copies nothing. Ownership passes to the
/// handle and on through @ref release to the frame or the engine, which calls
/// the release callback once it is done with the column. A handle that borrowed
/// a buffer and is never released frees it.
///
/// A column the plugin builds carries a bare type: no time unit and no
/// timezone. A Timestamp or Duration column therefore reads as
/// @ref TimeUnit "TimeUnit::Micro", the engine's default; a source that needs
/// another unit declares it in the schema it reports.
class Column {
   public:
    Column() noexcept = default;
    explicit Column(dftu_series* series) noexcept : series_(series) {}
    Column(const Column&) = delete;
    Column& operator=(const Column&) = delete;
    Column(Column&& other) noexcept : series_(other.release()) {}
    Column& operator=(Column&& other) noexcept {
        if (this != &other) {
            reset();
            series_ = other.release();
        }
        return *this;
    }
    ~Column() { reset(); }

    /// The column, or null when there is none. The engine takes it by pointer.
    dftu_series* get() const noexcept { return series_; }
    explicit operator bool() const noexcept { return series_ != nullptr; }
    bool is_null() const noexcept { return series_ == nullptr; }
    /// The element type, or @ref TypeId "TypeId::Unknown" for a null handle.
    TypeId dtype() const noexcept {
        return series_ != nullptr
                   ? static_cast<TypeId>(dftu_series_type(series_))
                   : TypeId::Unknown;
    }
    std::int64_t size() const noexcept {
        return series_ != nullptr ? dftu_series_length(series_) : 0;
    }

    /// Give the column up: the host or the frame owns it from here.
    dftu_series* release() noexcept {
        dftu_series* const out = series_;
        series_ = nullptr;
        return out;
    }
    void reset() noexcept {
        if (series_ != nullptr) {
            dftu_series_free(series_);
        }
        series_ = nullptr;
    }

    /// An int64 column over @p values, which it frees when the column dies.
    static Column from_int64(dstd::Vector<std::int64_t> values,
                             dstd::Vector<std::uint8_t> validity = {});
    /// An int32 column over @p values.
    static Column from_int32(dstd::Vector<std::int32_t> values,
                             dstd::Vector<std::uint8_t> validity = {});
    /// A uint64 column over @p values.
    static Column from_uint64(dstd::Vector<std::uint64_t> values,
                              dstd::Vector<std::uint8_t> validity = {});
    /// A double column over @p values.
    static Column from_double(dstd::Vector<double> values,
                              dstd::Vector<std::uint8_t> validity = {});
    /// A Timestamp column over @p values, read in
    /// @ref TimeUnit "TimeUnit::Micro" (see above; a copy of the values word
    /// for word, the type is the only difference from from_int64).
    static Column from_timestamp(dstd::Vector<std::int64_t> values,
                                 dstd::Vector<std::uint8_t> validity = {});
    /// A Duration column over @p values, read in
    /// @ref TimeUnit "TimeUnit::Micro".
    static Column from_duration(dstd::Vector<std::int64_t> values,
                                dstd::Vector<std::uint8_t> validity = {});
    /// A bool column over the bit-packed @p bits, one bit per row, least
    /// significant bit first, for @p rows rows. A @p bits too short for @p rows
    /// gives a null column rather than a read past its end.
    static Column from_bool(dstd::Vector<std::uint8_t> bits, std::int64_t rows,
                            dstd::Vector<std::uint8_t> validity = {});
    /// A String column over @p offsets (one more than the row count, the first
    /// 0) into the UTF-8 @p bytes. An int64 @p offsets gives a LargeString
    /// column, because the ABI reads int64 offsets only there.
    static Column from_strings(dstd::Vector<std::int32_t> offsets,
                               dstd::String bytes,
                               dstd::Vector<std::uint8_t> validity = {});
    static Column from_strings(dstd::Vector<std::int64_t> offsets,
                               dstd::String bytes,
                               dstd::Vector<std::uint8_t> validity = {});

    /// A column over memory the plugin does not own, such as a static table.
    /// It copies nothing and frees nothing; the memory must outlive the host's
    /// use of the column. The rows are the elements of the span.
    static Column from_static_int64(dstd::Span<const std::int64_t> values);
    static Column from_static_int32(dstd::Span<const std::int32_t> values);
    static Column from_static_uint64(dstd::Span<const std::uint64_t> values);
    static Column from_static_double(dstd::Span<const double> values);
    static Column from_static_timestamp(dstd::Span<const std::int64_t> values);
    static Column from_static_duration(dstd::Span<const std::int64_t> values);
    /// A Bool column over the bit-packed @p bits, one bit per row, for @p rows
    /// rows. A @p bits too short for @p rows gives a null column rather than a
    /// read past its end.
    static Column from_static_bool(dstd::Span<const std::uint8_t> bits,
                                   std::int64_t rows);
    /// A String column over @p offsets (one more than the row count, the first
    /// 0) into the UTF-8 @p bytes, both borrowed. Empty @p offsets gives a null
    /// column. An int64 @p offsets gives a LargeString column, because the ABI
    /// reads int64 offsets only there.
    static Column from_static_strings(dstd::Span<const std::int32_t> offsets,
                                      dstd::StringView bytes);
    static Column from_static_strings(dstd::Span<const std::int64_t> offsets,
                                      dstd::StringView bytes);

   private:
    dftu_series* series_ = nullptr;
};

/// Columns and the frame they make. Add each column by name, then @ref build.
/// Every added column is owned here until @ref build succeeds, so a failed
/// build frees everything made so far. Move only.
class FrameBuilder {
   public:
    FrameBuilder() = default;
    ~FrameBuilder() = default;
    FrameBuilder(const FrameBuilder&) = delete;
    FrameBuilder& operator=(const FrameBuilder&) = delete;
    FrameBuilder(FrameBuilder&&) = default;
    FrameBuilder& operator=(FrameBuilder&&) = default;

    /// Add a column. It owns the column from here; a null column is an error at
    /// @ref build. A second column of the same name is not refused, and the
    /// frame keeps both.
    FrameBuilder& add(dstd::StringView name, Column&& column) {
        names_.emplace_back(name);
        columns_.push_back(std::move(column));
        return *this;
    }
    std::size_t size() const noexcept { return columns_.size(); }

    /// Build the frame and give the columns to it. The columns must all have
    /// the same row count, and there must be at least one; anything else is an
    /// error and leaves the builder empty of what it made. A null column is an
    /// error too. On success the builder is empty and can take new columns.
    dstd::Result<OwnedDataFrame> build() {
        if (columns_.empty()) {
            return dstd::Unexpected(
                dstd::Error::from_format<"plugins: a frame needs a column">(
                    dstd::Condition::InvalidArgument));
        }
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            if (columns_[i].is_null()) {
                return dstd::Unexpected(
                    dstd::Error::from_format<"plugins: column '{}' is null">(
                        dstd::Condition::InvalidArgument, names_[i]));
            }
            if (columns_[i].size() != columns_[0].size()) {
                return dstd::Unexpected(dstd::Error::from_format<
                                        "plugins: column '{}' has {} rows, "
                                        "column '{}' has {}">(
                    dstd::Condition::InvalidArgument, names_[i],
                    columns_[i].size(), names_[0], columns_[0].size()));
            }
        }
        dstd::Vector<const char*> names;
        names.reserve(names_.size());
        for (const dstd::String& name : names_) names.push_back(name.c_str());
        dstd::Vector<dftu_series*> columns;
        columns.reserve(columns_.size());
        for (const Column& column : columns_) columns.push_back(column.get());
        dftu_dataframe* const frame =
            dftu_dataframe_new(names.data(), columns.data(),
                               static_cast<std::int32_t>(columns.size()));
        if (frame == nullptr) {
            return dstd::Unexpected(
                dstd::Error::from_format<"plugins: the frame was refused">(
                    dstd::Condition::InvalidArgument));
        }
        for (Column& column : columns_) column.release();
        columns_.clear();
        names_.clear();
        return OwnedDataFrame(frame);
    }

   private:
    dstd::Vector<dstd::String> names_;
    dstd::Vector<Column> columns_;
};

namespace detail {

/// The bytes a validity or bool bitmap for @p rows rows takes: one bit per row,
/// a whole number of bytes, 0 for no rows.
inline std::size_t bitmap_bytes(std::int64_t rows) noexcept {
    return rows > 0 ? static_cast<std::size_t>(rows + 7) / 8 : 0;
}

/// The @ref TypeId of the element type `T`, or @ref TypeId "TypeId::Unknown"
/// for a type that no fixed-width column holds.
template <class T>
constexpr TypeId type_id_of() noexcept {
    if constexpr (std::is_same_v<T, std::int8_t>) {
        return TypeId::Int8;
    } else if constexpr (std::is_same_v<T, std::int16_t>) {
        return TypeId::Int16;
    } else if constexpr (std::is_same_v<T, std::int32_t>) {
        return TypeId::Int32;
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
        return TypeId::Int64;
    } else if constexpr (std::is_same_v<T, std::uint8_t>) {
        return TypeId::Uint8;
    } else if constexpr (std::is_same_v<T, std::uint16_t>) {
        return TypeId::Uint16;
    } else if constexpr (std::is_same_v<T, std::uint32_t>) {
        return TypeId::Uint32;
    } else if constexpr (std::is_same_v<T, std::uint64_t>) {
        return TypeId::Uint64;
    } else if constexpr (std::is_same_v<T, float>) {
        return TypeId::Float32;
    } else if constexpr (std::is_same_v<T, double>) {
        return TypeId::Float64;
    } else {
        return TypeId::Unknown;
    }
}

/// The buffers a borrowed column holds on to once its caller has moved them in
/// here. A dstd::UniquePtr frees them in the release callback, never `delete`.
template <class T>
struct FlatBuffers {
    dstd::Vector<T> values;
    dstd::Vector<std::uint8_t> validity;
};

template <class T>
void release_flat(void* buffers) noexcept {
    dstd::UniquePtr<FlatBuffers<T>> hold(static_cast<FlatBuffers<T>*>(buffers));
}

/// Borrow @p values for @p rows rows of @p type. A validity bitmap that is too
/// short for @p rows is refused, so the engine never reads past it.
template <class T>
Column borrow_flat(dftu_dtype type, std::int64_t rows, dstd::Vector<T> values,
                   dstd::Vector<std::uint8_t> validity) {
    if (rows < 0 ||
        (!validity.empty() && validity.size() < bitmap_bytes(rows))) {
        return Column();
    }
    dstd::UniquePtr<FlatBuffers<T>> hold = dstd::make_unique<FlatBuffers<T>>();
    hold->values = std::move(values);
    hold->validity = std::move(validity);
    FlatBuffers<T>* const raw = hold.release();
    return Column(dftu_series_new_flat_borrowed(
        type, raw->values.data(), rows,
        raw->validity.empty() ? nullptr : raw->validity.data(), release_flat<T>,
        raw));
}

/// The buffers a borrowed string column holds on to: the offsets, the bytes and
/// the validity bitmap of their rows.
template <class Offset>
struct StringBuffers {
    dstd::Vector<Offset> offsets;
    dstd::String bytes;
    dstd::Vector<std::uint8_t> validity;
};

template <class Offset>
void release_string(void* buffers) noexcept {
    dstd::UniquePtr<StringBuffers<Offset>> hold(
        static_cast<StringBuffers<Offset>*>(buffers));
}

template <class Offset>
Column borrow_string(dftu_dtype type, dstd::Vector<Offset> offsets,
                     dstd::String bytes, dstd::Vector<std::uint8_t> validity) {
    if (offsets.empty()) {
        return Column();
    }
    const std::int64_t rows = static_cast<std::int64_t>(offsets.size()) - 1;
    if (!validity.empty() && validity.size() < bitmap_bytes(rows)) {
        return Column();
    }
    dstd::UniquePtr<StringBuffers<Offset>> hold =
        dstd::make_unique<StringBuffers<Offset>>();
    hold->offsets = std::move(offsets);
    hold->bytes = std::move(bytes);
    hold->validity = std::move(validity);
    StringBuffers<Offset>* const raw = hold.release();
    return Column(dftu_series_new_string_borrowed(
        type, raw->offsets.data(), raw->bytes.data(),
        static_cast<std::int64_t>(raw->bytes.size()), rows,
        raw->validity.empty() ? nullptr : raw->validity.data(),
        release_string<Offset>, raw));
}

}  // namespace detail

namespace detail {
class ColumnViewAccess;
}  // namespace detail

/// A borrowed view of one column of a frame the host owns. It holds no
/// reference, copies no data and allocates nothing, so it is built in place and
/// stays valid only while the frame, or the batch it came from, is alive.
///
/// Which call needs which encoding: @ref type, @ref size, @ref is_null and
/// @ref string_at read a column in any encoding, because the engine resolves a
/// DICTIONARY or SELECTION base per row. @ref values needs a FLAT column, and
/// @ref flat gives one: it is the column itself when the column is already
/// FLAT, and a materialized copy of its rows otherwise.
class ColumnView {
   public:
    ColumnView() noexcept = default;
    /// A view of @p series, which stays owned by the caller and must outlive
    /// the view. NULL views no column.
    explicit ColumnView(const dftu_series* series) noexcept : series_(series) {}

    /// The element type of the column, or @ref TypeId "TypeId::Unknown" for a
    /// view of no column.
    TypeId type() const noexcept {
        return series_ != nullptr
                   ? static_cast<TypeId>(dftu_series_type(series_))
                   : TypeId::Unknown;
    }
    /// The number of rows.
    std::int64_t size() const noexcept {
        return series_ != nullptr ? dftu_series_length(series_) : 0;
    }
    /// Whether row @p row is null.
    bool is_null(std::int64_t row) const noexcept {
        return series_ != nullptr && dftu_series_is_null(series_, row) != 0;
    }

    /// Whether row @p row of a FLAT Bool column is true; false for a null row,
    /// a column of another type, a column that is not FLAT and a view of no
    /// column. Call @ref flat on a DICTIONARY, SELECTION, CONSTANT or CHUNKED
    /// column first.
    bool boolean_at(std::int64_t row) const noexcept {
        if (series_ == nullptr || type() != TypeId::Bool || row < 0 ||
            row >= size() || is_null(row))
            return false;
        const void* const data = dftu_series_data(series_);
        if (data == nullptr) return false;
        const auto* const bits = static_cast<const std::uint8_t*>(data);
        return ((bits[row >> 3] >> (row & 7)) & 1u) != 0;
    }

    /// The values of row 0 upwards of a FLAT fixed-width column, as a borrowed
    /// span: no copy, no allocation. `T` must be the column's element type
    /// (`std::int64_t` for an Int64, Timestamp or Duration column, `double` for
    /// a Float64 column). The span is empty, and never a fatal error, for
    /// another `T`, for a view of no column, and for a column that is not
    /// FLAT, in every build. A Bool column is bit-packed, so it has no
    /// `values<T>`; call @ref flat on a DICTIONARY, SELECTION, CONSTANT or
    /// CHUNKED column first.
    template <class T>
    dstd::Span<const T> values() const noexcept {
        if (series_ == nullptr ||
            detail::type_id_of<T>() != physical_type(type()))
            return {};
        const void* const data = dftu_series_data(series_);
        if (data == nullptr) return {};
        return dstd::Span<const T>(static_cast<const T*>(data),
                                   static_cast<std::size_t>(size()));
    }

    /// The bytes of row @p row of a String, Binary, LargeString or
    /// LargeBinary column of any encoding, borrowed: no copy, no allocation.
    /// Empty for a null row and for a column of another type.
    dstd::StringView string_at(std::int64_t row) const noexcept {
        std::int64_t size = 0;
        const char* const bytes = dftu_series_string_at(series_, row, &size);
        return bytes != nullptr
                   ? dstd::StringView(bytes, static_cast<std::size_t>(size))
                   : dstd::StringView();
    }

    /// The column as a FLAT one. A column that is already FLAT comes back as
    /// the same column, sharing its buffers (a reference count, no data copy);
    /// any other encoding comes back as a copy of its rows. The result owns a
    /// handle, so hold it for as long as the flat values are read.
    dstd::Result<Column> flat() const {
        if (series_ == nullptr) {
            return dstd::Unexpected(
                dstd::Error::from_format<"dataframe: no column to flatten">(
                    dstd::Condition::InvalidArgument));
        }
        dftu_series* flat = dftu_series_encoding(series_) ==
                                    static_cast<std::int32_t>(Encoding::Flat)
                                ? dftu_series_share(series_)
                                : dftu_series_materialize(series_);
        if (flat == nullptr) {
            return dstd::Unexpected(dstd::Error::from_format<
                                    "dataframe: the column was not allocated">(
                dstd::Condition::OutOfMemory));
        }
        return Column(flat);
    }

    /// The column as an owned handle in the encoding it has, sharing its
    /// buffers (a reference count, no data copy). Hold the handle for as long
    /// as the column is read, which is how a stage passes a column through
    /// without flattening it.
    dstd::Result<Column> share() const {
        if (series_ == nullptr) {
            return dstd::Unexpected(
                dstd::Error::from_format<"dataframe: no column to share">(
                    dstd::Condition::InvalidArgument));
        }
        dftu_series* const held = dftu_series_share(series_);
        if (held == nullptr) {
            return dstd::Unexpected(
                dstd::Error::from_format<"dataframe: the column was not held">(
                    dstd::Condition::OutOfMemory));
        }
        return Column(held);
    }

   private:
    friend class detail::ColumnViewAccess;
    const dftu_series* series_ = nullptr;
};

namespace detail {

/// The C column a @ref ColumnView borrows. SDK plumbing, not plugin surface.
class ColumnViewAccess {
   public:
    static const dftu_series* get(const ColumnView& view) noexcept {
        return view.series_;
    }
};

/// @see ColumnViewAccess
inline const dftu_series* column_series(const ColumnView& view) noexcept {
    return ColumnViewAccess::get(view);
}

}  // namespace detail

/// A borrowed view of a frame the host owns. It holds no reference and
/// allocates nothing, so it is built in place and stays valid only while the
/// frame is alive. Every column it gives is a @ref ColumnView.
class FrameView {
   public:
    FrameView() noexcept = default;
    /// A view of @p frame, which stays owned by the caller and must outlive the
    /// view.
    explicit FrameView(const dftu_dataframe* frame) noexcept : frame_(frame) {}

    /// The number of columns.
    std::int32_t column_count() const noexcept {
        return frame_ != nullptr ? dftu_dataframe_num_columns(frame_) : 0;
    }
    /// The number of rows every column has.
    std::int64_t row_count() const noexcept {
        return frame_ != nullptr ? dftu_dataframe_num_rows(frame_) : 0;
    }
    /// The name of column @p i, borrowed for as long as the frame lives; empty
    /// for an index out of range.
    dstd::StringView name(std::int32_t i) const noexcept {
        const char* const text =
            frame_ != nullptr ? dftu_dataframe_column_name(frame_, i) : nullptr;
        return text != nullptr ? dstd::StringView(text) : dstd::StringView();
    }

    /// Column @p i, or a view of no column for an index out of range.
    ColumnView column(std::int32_t i) const noexcept {
        return ColumnView(
            frame_ != nullptr ? dftu_dataframe_column_at(frame_, i) : nullptr);
    }

    /// The column named @p name, or nothing when the frame has no such column.
    dstd::Optional<ColumnView> column(dstd::StringView name) const noexcept {
        const std::int32_t count = column_count();
        for (std::int32_t i = 0; i < count; ++i) {
            if (this->name(i) == name) return column(i);
        }
        return dstd::nullopt;
    }

   private:
    const dftu_dataframe* frame_ = nullptr;
};

inline FrameView OwnedDataFrame::view() const noexcept {
    return FrameView(frame_);
}

inline Column Column::from_int64(dstd::Vector<std::int64_t> values,
                                 dstd::Vector<std::uint8_t> validity) {
    const std::int64_t rows = static_cast<std::int64_t>(values.size());
    return detail::borrow_flat(DFTU_TYPE_INT64, rows, std::move(values),
                               std::move(validity));
}

inline Column Column::from_int32(dstd::Vector<std::int32_t> values,
                                 dstd::Vector<std::uint8_t> validity) {
    const std::int64_t rows = static_cast<std::int64_t>(values.size());
    return detail::borrow_flat(DFTU_TYPE_INT32, rows, std::move(values),
                               std::move(validity));
}

inline Column Column::from_uint64(dstd::Vector<std::uint64_t> values,
                                  dstd::Vector<std::uint8_t> validity) {
    const std::int64_t rows = static_cast<std::int64_t>(values.size());
    return detail::borrow_flat(DFTU_TYPE_UINT64, rows, std::move(values),
                               std::move(validity));
}

inline Column Column::from_double(dstd::Vector<double> values,
                                  dstd::Vector<std::uint8_t> validity) {
    const std::int64_t rows = static_cast<std::int64_t>(values.size());
    return detail::borrow_flat(DFTU_TYPE_FLOAT64, rows, std::move(values),
                               std::move(validity));
}

inline Column Column::from_timestamp(dstd::Vector<std::int64_t> values,
                                     dstd::Vector<std::uint8_t> validity) {
    const std::int64_t rows = static_cast<std::int64_t>(values.size());
    return detail::borrow_flat(DFTU_TYPE_TIMESTAMP, rows, std::move(values),
                               std::move(validity));
}

inline Column Column::from_duration(dstd::Vector<std::int64_t> values,
                                    dstd::Vector<std::uint8_t> validity) {
    const std::int64_t rows = static_cast<std::int64_t>(values.size());
    return detail::borrow_flat(DFTU_TYPE_DURATION, rows, std::move(values),
                               std::move(validity));
}

inline Column Column::from_bool(dstd::Vector<std::uint8_t> bits,
                                std::int64_t rows,
                                dstd::Vector<std::uint8_t> validity) {
    if (rows < 0 || bits.size() < detail::bitmap_bytes(rows)) {
        return Column();
    }
    return detail::borrow_flat(DFTU_TYPE_BOOL, rows, std::move(bits),
                               std::move(validity));
}

inline Column Column::from_strings(dstd::Vector<std::int32_t> offsets,
                                   dstd::String bytes,
                                   dstd::Vector<std::uint8_t> validity) {
    return detail::borrow_string(DFTU_TYPE_STRING, std::move(offsets),
                                 std::move(bytes), std::move(validity));
}

inline Column Column::from_strings(dstd::Vector<std::int64_t> offsets,
                                   dstd::String bytes,
                                   dstd::Vector<std::uint8_t> validity) {
    return detail::borrow_string(DFTU_TYPE_LARGE_STRING, std::move(offsets),
                                 std::move(bytes), std::move(validity));
}

inline Column Column::from_static_int64(dstd::Span<const std::int64_t> values) {
    return Column(dftu_series_new_flat_borrowed(
        DFTU_TYPE_INT64, values.data(),
        static_cast<std::int64_t>(values.size()), nullptr, nullptr, nullptr));
}

inline Column Column::from_static_int32(dstd::Span<const std::int32_t> values) {
    return Column(dftu_series_new_flat_borrowed(
        DFTU_TYPE_INT32, values.data(),
        static_cast<std::int64_t>(values.size()), nullptr, nullptr, nullptr));
}

inline Column Column::from_static_uint64(
    dstd::Span<const std::uint64_t> values) {
    return Column(dftu_series_new_flat_borrowed(
        DFTU_TYPE_UINT64, values.data(),
        static_cast<std::int64_t>(values.size()), nullptr, nullptr, nullptr));
}

inline Column Column::from_static_double(dstd::Span<const double> values) {
    return Column(dftu_series_new_flat_borrowed(
        DFTU_TYPE_FLOAT64, values.data(),
        static_cast<std::int64_t>(values.size()), nullptr, nullptr, nullptr));
}

inline Column Column::from_static_timestamp(
    dstd::Span<const std::int64_t> values) {
    return Column(dftu_series_new_flat_borrowed(
        DFTU_TYPE_TIMESTAMP, values.data(),
        static_cast<std::int64_t>(values.size()), nullptr, nullptr, nullptr));
}

inline Column Column::from_static_duration(
    dstd::Span<const std::int64_t> values) {
    return Column(dftu_series_new_flat_borrowed(
        DFTU_TYPE_DURATION, values.data(),
        static_cast<std::int64_t>(values.size()), nullptr, nullptr, nullptr));
}

inline Column Column::from_static_bool(dstd::Span<const std::uint8_t> bits,
                                       std::int64_t rows) {
    if (rows < 0 || bits.size() < detail::bitmap_bytes(rows)) return Column();
    return Column(dftu_series_new_flat_borrowed(
        DFTU_TYPE_BOOL, bits.data(), rows, nullptr, nullptr, nullptr));
}

inline Column Column::from_static_strings(
    dstd::Span<const std::int32_t> offsets, dstd::StringView bytes) {
    if (offsets.empty()) return Column();
    return Column(dftu_series_new_string_borrowed(
        DFTU_TYPE_STRING, offsets.data(), bytes.data(),
        static_cast<std::int64_t>(bytes.size()),
        static_cast<std::int64_t>(offsets.size()) - 1, nullptr, nullptr,
        nullptr));
}

inline Column Column::from_static_strings(
    dstd::Span<const std::int64_t> offsets, dstd::StringView bytes) {
    if (offsets.empty()) return Column();
    return Column(dftu_series_new_string_borrowed(
        DFTU_TYPE_LARGE_STRING, offsets.data(), bytes.data(),
        static_cast<std::int64_t>(bytes.size()),
        static_cast<std::int64_t>(offsets.size()) - 1, nullptr, nullptr,
        nullptr));
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_PLUGIN_FRAME_H
