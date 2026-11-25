// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/errors.h>
#include <dftracer/utils/dataframe/internal/nested_view.h>

#include <algorithm>

namespace dftracer::utils::dataframe {

namespace {

bool is_list_like(TypeId t) {
    return t == TypeId::List || t == TypeId::LargeList ||
           t == TypeId::FixedSizeList || t == TypeId::Map;
}

}  // namespace

bool NestedPiece::is_null(std::int64_t i) const noexcept {
    return dftu_series_is_null(col_, i) != 0;
}

ValueRange NestedPiece::values_of(std::int64_t i) const noexcept {
    switch (col_->type) {
        case TypeId::FixedSizeList: {
            const std::int64_t n = col_->fixed_size();
            return {i * n, (i + 1) * n};
        }
        case TypeId::LargeList: {
            const auto* o =
                reinterpret_cast<const std::int64_t*>(col_->offsets.data());
            return {o[i], o[i + 1]};
        }
        default: {
            const auto* o =
                reinterpret_cast<const std::int32_t*>(col_->offsets.data());
            return {o[i], o[i + 1]};
        }
    }
}

std::int64_t NestedPiece::num_children() const noexcept {
    if (is_list_like(col_->type)) return col_->child() ? 1 : 0;
    return static_cast<std::int64_t>(col_->num_fields());
}

const dftu_series& NestedPiece::child(std::int64_t i) const noexcept {
    if (is_list_like(col_->type)) return *col_->child();
    return *col_->nested[static_cast<std::size_t>(i)].series;
}

dstd::StringView NestedPiece::field_name(std::int64_t i) const noexcept {
    if (!col_->has_fields()) return {};
    const dstd::String& n = col_->nested[static_cast<std::size_t>(i)].name;
    return {n.data(), n.size()};
}

dstd::Result<NestedPiece> nested_whole(const dftu_series& col) {
    if (!is_list_like(col.type) && col.type != TypeId::Struct)
        return invalid_argument<"not a nested column">();
    if (col.encoding == Encoding::Flat) return NestedPiece{&col, {}, 0};
    dftu_series* flat = dftu_series_materialize(&col);
    if (!flat)
        return invalid_argument<"a nested column that cannot be made flat">();
    dstd::SharedPtr<dftu_series> keep = adopt_series(flat);
    return NestedPiece{flat, std::move(keep), 0};
}

dstd::Result<NestedPiece> nested_whole(const Series& col) {
    return nested_whole(*col.handle());
}

dstd::Result<dstd::Vector<NestedPiece>> nested_pieces(const dftu_series& col) {
    dstd::Vector<NestedPiece> out;
    if (!col.is_chunked()) {
        auto whole = nested_whole(col);
        if (!whole) return whole.error();
        out.push_back(std::move(*whole));
        return out;
    }
    const auto* starts = reinterpret_cast<const std::int64_t*>(col.data.data());
    out.reserve(col.nested.size());
    for (std::size_t k = 0; k < col.nested.size(); ++k) {
        const dstd::SharedPtr<dftu_series>& c = col.nested[k].series;
        if (c->encoding == Encoding::Flat &&
            (is_list_like(c->type) || c->type == TypeId::Struct)) {
            out.push_back(NestedPiece{c.get(), c, starts[k]});
            continue;
        }
        auto whole = nested_whole(*c);
        if (!whole) return whole.error();
        out.push_back(
            NestedPiece{&whole->column(), std::move(whole->keep_), starts[k]});
    }
    return out;
}

dstd::Result<dstd::Vector<NestedPiece>> nested_pieces(const Series& col) {
    return nested_pieces(*col.handle());
}

dstd::Result<NestedPiece> nested_row(const dftu_series& col, std::int64_t i,
                                     std::int64_t& local) {
    const dftu_series* c = &col;
    std::int64_t row = i;
    while (c->encoding != Encoding::Flat) {
        if (c->is_chunked()) {
            std::int64_t in_chunk = 0;
            const dftu_series& chunk = c->chunk_at(row, in_chunk);
            c = &chunk;
            row = in_chunk;
            continue;
        }
        if ((c->encoding != Encoding::Selection &&
             c->encoding != Encoding::Dictionary) ||
            !c->child())
            return invalid_argument<"a nested column of no flat base">();
        const std::int64_t at =
            c->encoding == Encoding::Selection
                ? reinterpret_cast<const std::int64_t*>(c->data.data())[row]
                : reinterpret_cast<const std::int32_t*>(c->data.data())[row];
        // The own bitmap first: an index under a null row may hold anything.
        const bool own_null =
            c->validity.block() &&
            !((c->validity.data()[row >> 3] >> (row & 7)) & 1);
        if (own_null || at < 0)
            return invalid_argument<"a null row has no base row">();
        if (at >= c->child()->length)
            return invalid_argument<"an index past its base">();
        c = c->child().get();
        row = at;
    }
    if (!is_list_like(c->type) && c->type != TypeId::Struct)
        return invalid_argument<"not a nested column">();
    local = row;
    return NestedPiece{c, {}, 0};
}

dstd::Result<NestedPiece> nested_row(const Series& col, std::int64_t i,
                                     std::int64_t& local) {
    return nested_row(*col.handle(), i, local);
}

const NestedPiece& piece_at(dstd::Span<const NestedPiece> pieces,
                            std::int64_t i, std::int64_t& local) noexcept {
    const auto it =
        std::upper_bound(pieces.begin(), pieces.end(), i,
                         [](std::int64_t row, const NestedPiece& p) {
                             return row < p.row_base();
                         });
    const NestedPiece& p = *(it - 1);
    local = i - p.row_base();
    return p;
}

}  // namespace dftracer::utils::dataframe
