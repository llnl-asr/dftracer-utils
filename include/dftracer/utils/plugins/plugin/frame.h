// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_FRAME_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_FRAME_H

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <cstddef>
#include <cstdint>
#include <utility>

/// A frame the host owns, a column the plugin builds, and the builder that
/// puts columns into a frame. A source, a node, a file format and a UDF all
/// give the host a frame or a column: @ref Column takes the plugin's buffers by
/// move and hands them to the engine without a copy, and @ref FrameBuilder
/// names columns and builds the frame. Every call reports failure as a @ref
/// dstd::Result, and a column made on the way to a failed build is freed by its
/// owner.
namespace dftracer::utils::plugins {

/// A native columnar result carried across the ABI as a dftu_dataframe handle
/// (the dataframe engine's own boundary type), freed through the dataframe C
/// ABI. Lets an engine-backed result (the aggregation accumulator) cross as our
/// own @ref dftracer::utils::dataframe::DataFrame "DataFrame" with no Arrow
/// round-trip. The one frame owner type of the plugin surface: the frame that a
/// named result, a source, a node or a format hands back.
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
/// A column the plugin builds carries a bare dtype: no time unit and no
/// timezone. A Timestamp or Duration column therefore reads as
/// DFTU_TIME_UNIT_MICRO, the engine's default; a source that needs another unit
/// declares it in the schema it reports (dftu_schema_add_field).
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
    std::int32_t dtype() const noexcept {
        return series_ != nullptr ? dftu_series_type(series_)
                                  : DFTU_TYPE_UNKNOWN;
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
    /// A Timestamp column over @p values, read in DFTU_TIME_UNIT_MICRO (see
    /// above; a copy of the values word for word, the type is the only
    /// difference from from_int64).
    static Column from_timestamp(dstd::Vector<std::int64_t> values,
                                 dstd::Vector<std::uint8_t> validity = {});
    /// A Duration column over @p values, read in DFTU_TIME_UNIT_MICRO.
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
    /// use of the column.
    static Column from_static_int64(const std::int64_t* data,
                                    std::int64_t rows);
    static Column from_static_int32(const std::int32_t* data,
                                    std::int64_t rows);
    static Column from_static_uint64(const std::uint64_t* data,
                                     std::int64_t rows);
    static Column from_static_double(const double* data, std::int64_t rows);
    static Column from_static_bool(const std::uint8_t* bits, std::int64_t rows);
    static Column from_static_timestamp(const std::int64_t* data,
                                        std::int64_t rows);
    static Column from_static_duration(const std::int64_t* data,
                                       std::int64_t rows);
    /// @p offsets holds one more value than @p rows, and @p bytes_size bytes
    /// follow @p bytes.
    static Column from_static_strings(const std::int32_t* offsets,
                                      const char* bytes,
                                      std::int64_t bytes_size,
                                      std::int64_t rows);
    static Column from_static_strings(const std::int64_t* offsets,
                                      const char* bytes,
                                      std::int64_t bytes_size,
                                      std::int64_t rows);

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
                make_error(DFTU_E_INVALID, "plugins: a frame needs a column"));
        }
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            if (columns_[i].is_null()) {
                return dstd::Unexpected(make_error(
                    DFTU_E_INVALID,
                    dstd::fmt<"plugins: column '{}' is null">(names_[i])));
            }
            if (columns_[i].size() != columns_[0].size()) {
                return dstd::Unexpected(make_error(
                    DFTU_E_INVALID,
                    dstd::fmt<"plugins: column '{}' has {} rows, column '{}' "
                              "has {}">(names_[i], columns_[i].size(),
                                        names_[0], columns_[0].size())));
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
                make_error(DFTU_E_INVALID, "plugins: the frame was refused"));
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

inline Column Column::from_static_int64(const std::int64_t* data,
                                        std::int64_t rows) {
    return Column(dftu_series_new_flat_borrowed(DFTU_TYPE_INT64, data, rows,
                                                nullptr, nullptr, nullptr));
}

inline Column Column::from_static_int32(const std::int32_t* data,
                                        std::int64_t rows) {
    return Column(dftu_series_new_flat_borrowed(DFTU_TYPE_INT32, data, rows,
                                                nullptr, nullptr, nullptr));
}

inline Column Column::from_static_uint64(const std::uint64_t* data,
                                         std::int64_t rows) {
    return Column(dftu_series_new_flat_borrowed(DFTU_TYPE_UINT64, data, rows,
                                                nullptr, nullptr, nullptr));
}

inline Column Column::from_static_double(const double* data,
                                         std::int64_t rows) {
    return Column(dftu_series_new_flat_borrowed(DFTU_TYPE_FLOAT64, data, rows,
                                                nullptr, nullptr, nullptr));
}

inline Column Column::from_static_timestamp(const std::int64_t* data,
                                            std::int64_t rows) {
    return Column(dftu_series_new_flat_borrowed(DFTU_TYPE_TIMESTAMP, data, rows,
                                                nullptr, nullptr, nullptr));
}

inline Column Column::from_static_duration(const std::int64_t* data,
                                           std::int64_t rows) {
    return Column(dftu_series_new_flat_borrowed(DFTU_TYPE_DURATION, data, rows,
                                                nullptr, nullptr, nullptr));
}

inline Column Column::from_static_bool(const std::uint8_t* bits,
                                       std::int64_t rows) {
    return Column(dftu_series_new_flat_borrowed(DFTU_TYPE_BOOL, bits, rows,
                                                nullptr, nullptr, nullptr));
}

inline Column Column::from_static_strings(const std::int32_t* offsets,
                                          const char* bytes,
                                          std::int64_t bytes_size,
                                          std::int64_t rows) {
    return Column(dftu_series_new_string_borrowed(DFTU_TYPE_STRING, offsets,
                                                  bytes, bytes_size, rows,
                                                  nullptr, nullptr, nullptr));
}

inline Column Column::from_static_strings(const std::int64_t* offsets,
                                          const char* bytes,
                                          std::int64_t bytes_size,
                                          std::int64_t rows) {
    return Column(dftu_series_new_string_borrowed(
        DFTU_TYPE_LARGE_STRING, offsets, bytes, bytes_size, rows, nullptr,
        nullptr, nullptr));
}

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_PLUGIN_FRAME_H
