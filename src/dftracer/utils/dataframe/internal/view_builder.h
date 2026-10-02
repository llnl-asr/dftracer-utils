#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_VIEW_BUILDER_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_VIEW_BUILDER_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/dataframe/buffer.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

struct dftu_series;

namespace dftracer::utils::dataframe {

/// Builds a VIEW String or Binary column row by row. A long value (more than
/// 12 bytes) is a view into a buffer the column keeps alive; only inline
/// values are copied. Buffers are deduplicated by data pointer.
///
/// Not thread safe. A value longer than INT32_MAX bytes cannot be a view and
/// throws std::length_error. A value of a flat column that starts past
/// INT32_MAX bytes of its buffer cannot have a view offset, so it is copied
/// into a buffer owned by the builder.
class ViewBuilder {
   public:
    void reserve(std::int64_t rows);

    /// The buffer's index; a buffer already added (same data pointer) returns
    /// its index.
    std::int32_t add_buffer(std::shared_ptr<Buffer> buffer);

    /// A row viewing `bytes`, which must lie inside buffer `buffer_index`.
    void append(std::string_view bytes, std::int32_t buffer_index);

    /// A row of at most 12 bytes, copied into the view.
    void append_inline(std::string_view bytes);

    void append_null();

    /// Row `row` of a String, Binary, LargeString or LargeBinary column in any
    /// encoding (flat, view, dictionary, selection), resolved to its value.
    void append_row(const dftu_series& col, std::int64_t row);

    /// Every row of `col`; a view column is copied as 16-byte views with its
    /// buffer indexes remapped.
    void append_column(const dftu_series& col);

    std::int64_t length() const noexcept {
        return static_cast<std::int64_t>(views_.size() / 16);
    }

    /// The column; the builder is empty afterwards. `type` is String or
    /// Binary.
    Series finish(TypeId type, bool json);

   private:
    std::uint8_t* new_view();
    std::int32_t cached_buffer(const std::shared_ptr<Buffer>& buffer);
    void append_flat(const dftu_series& col, std::int64_t row);
    void append_view_row(const dftu_series& col, std::int64_t row);

    std::vector<std::uint8_t> views_;
    std::vector<std::shared_ptr<Buffer>> buffers_;
    ankerl::unordered_dense::map<const std::uint8_t*, std::int32_t> index_of_;
    std::vector<std::int64_t> null_rows_;
    const std::uint8_t* last_data_ = nullptr;
    std::int32_t last_index_ = 0;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_VIEW_BUILDER_H
