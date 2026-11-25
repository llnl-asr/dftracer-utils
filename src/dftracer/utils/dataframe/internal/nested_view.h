// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_NESTED_VIEW_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_NESTED_VIEW_H

#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::dataframe {

/// The row range [@p lo, @p hi) of one list's values in its child column.
struct ValueRange {
    std::int64_t lo = 0;
    std::int64_t hi = 0;
};

/// One FLAT piece of a nested column (List, LargeList, FixedSizeList, Map,
/// Struct): the column itself, one of its chunks, or a joined copy. Only
/// @ref nested_whole and @ref nested_pieces make one, so a reader of list
/// offsets or children through it never meets a CHUNKED, SELECTION or
/// DICTIONARY column. A piece of a FLAT input borrows it: the input must
/// outlive the piece.
class NestedPiece {
   public:
    const dftu_series& column() const noexcept { return *col_; }
    TypeId type() const noexcept { return col_->type; }
    std::int64_t length() const noexcept { return col_->length; }
    /// The first row of this piece in the whole column.
    std::int64_t row_base() const noexcept { return base_; }
    bool is_null(std::int64_t i) const noexcept;
    /// Row @p i's values in @ref child(0); a List, LargeList, Map or
    /// FixedSizeList piece only.
    ValueRange values_of(std::int64_t i) const noexcept;
    /// 1 for a list-like piece, the field count for a Struct, else 0.
    std::int64_t num_children() const noexcept;
    /// Child @p i: the values of a list-like piece (0 only) or Struct field
    /// @p i. Borrowed from the piece.
    const dftu_series& child(std::int64_t i) const noexcept;
    /// Struct field @p i's name; empty for a list-like piece.
    dstd::StringView field_name(std::int64_t i) const noexcept;

   private:
    friend dstd::Result<NestedPiece> nested_whole(const dftu_series& col);
    friend dstd::Result<dstd::Vector<NestedPiece>> nested_pieces(
        const dftu_series& col);
    friend dstd::Result<NestedPiece> nested_row(const dftu_series& col,
                                                std::int64_t i,
                                                std::int64_t& local);
    NestedPiece(const dftu_series* col, dstd::SharedPtr<dftu_series> keep,
                std::int64_t base)
        : col_(col), keep_(std::move(keep)), base_(base) {}

    const dftu_series* col_;
    dstd::SharedPtr<dftu_series> keep_;  // null when the piece borrows
    std::int64_t base_;
};

/// @p col as one FLAT piece: borrows a FLAT column, else joins a CHUNKED
/// one or materializes a SELECTION or DICTIONARY one (a copy). An error for a
/// column that is not nested or cannot be made flat.
dstd::Result<NestedPiece> nested_whole(const dftu_series& col);
dstd::Result<NestedPiece> nested_whole(const Series& col);

/// The FLAT pieces of @p col in row order, with no copy for a FLAT column or
/// a CHUNKED one of FLAT chunks: the column itself, each chunk, or one
/// materialized copy of a SELECTION or DICTIONARY column.
dstd::Result<dstd::Vector<NestedPiece>> nested_pieces(const dftu_series& col);
dstd::Result<dstd::Vector<NestedPiece>> nested_pieces(const Series& col);

/// The FLAT piece that holds row @p i of @p col, with no copy: a chunk of a
/// CHUNKED column, or the base row a SELECTION index or DICTIONARY code points
/// at; @p local gets the row inside it. An error when row @p i is null in a
/// SELECTION or DICTIONARY or the column is not nested. The piece borrows
/// @p col's buffers, so @p col must outlive it.
dstd::Result<NestedPiece> nested_row(const dftu_series& col, std::int64_t i,
                                     std::int64_t& local);
dstd::Result<NestedPiece> nested_row(const Series& col, std::int64_t i,
                                     std::int64_t& local);

/// The piece holding row @p i of the whole column; @p local gets the row
/// inside it. @p pieces are in row order and hold row @p i.
const NestedPiece& piece_at(dstd::Span<const NestedPiece> pieces,
                            std::int64_t i, std::int64_t& local) noexcept;

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_NESTED_VIEW_H
