#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/view_builder.h>

#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace dftracer::utils::dataframe {

namespace {

constexpr std::size_t VIEW_BYTES = 16;
constexpr std::int32_t INLINE_MAX = 12;
constexpr std::size_t PREFIX_BYTES = 4;
constexpr std::int64_t INT32_LIMIT = std::numeric_limits<std::int32_t>::max();

bool own_null(const dftu_series& c, std::int64_t i) {
    return c.validity && !((c.validity->data()[i >> 3] >> (i & 7)) & 1);
}

}  // namespace

void ViewBuilder::reserve(std::int64_t rows) {
    if (rows > 0)
        views_.reserve(views_.size() +
                       static_cast<std::size_t>(rows) * VIEW_BYTES);
}

std::uint8_t* ViewBuilder::new_view() {
    const std::size_t at = views_.size();
    views_.resize(at + VIEW_BYTES);
    return views_.data() + at;
}

std::int32_t ViewBuilder::add_buffer(std::shared_ptr<Buffer> buffer) {
    const std::uint8_t* key = buffer->data();
    auto [it, fresh] =
        index_of_.try_emplace(key, static_cast<std::int32_t>(buffers_.size()));
    if (fresh) {
        if (buffers_.size() >= static_cast<std::size_t>(INT32_LIMIT)) {
            index_of_.erase(it);
            throw std::length_error("view column: too many buffers");
        }
        buffers_.push_back(std::move(buffer));
    }
    return it->second;
}

std::int32_t ViewBuilder::cached_buffer(const std::shared_ptr<Buffer>& buffer) {
    if (buffer->data() != last_data_) {
        last_index_ = add_buffer(buffer);
        last_data_ = buffer->data();
    }
    return last_index_;
}

void ViewBuilder::append_inline(std::string_view bytes) {
    if (bytes.size() > static_cast<std::size_t>(INLINE_MAX))
        throw std::length_error("view column: inline value over 12 bytes");
    std::uint8_t* v = new_view();
    const auto size = static_cast<std::int32_t>(bytes.size());
    std::memcpy(v, &size, sizeof(size));
    if (!bytes.empty())
        std::memcpy(v + PREFIX_BYTES, bytes.data(), bytes.size());
}

void ViewBuilder::append(std::string_view bytes, std::int32_t buffer_index) {
    if (bytes.size() <= static_cast<std::size_t>(INLINE_MAX))
        return append_inline(bytes);
    if (bytes.size() > static_cast<std::size_t>(INT32_LIMIT))
        throw std::length_error("view column: value over INT32_MAX bytes");
    const std::int64_t offset =
        reinterpret_cast<const std::uint8_t*>(bytes.data()) -
        buffers_[static_cast<std::size_t>(buffer_index)]->data();
    if (offset < 0 || offset > INT32_LIMIT)
        throw std::length_error("view column: offset outside int32");
    std::uint8_t* v = new_view();
    const auto size = static_cast<std::int32_t>(bytes.size());
    const auto off = static_cast<std::int32_t>(offset);
    std::memcpy(v, &size, sizeof(size));
    std::memcpy(v + PREFIX_BYTES, bytes.data(), PREFIX_BYTES);
    std::memcpy(v + 8, &buffer_index, sizeof(buffer_index));
    std::memcpy(v + 12, &off, sizeof(off));
}

void ViewBuilder::append_null() {
    null_rows_.push_back(length());
    new_view();
}

void ViewBuilder::append_flat(const dftu_series& c, std::int64_t row) {
    const auto idx = static_cast<std::size_t>(row);
    std::int64_t begin, end;
    if (c.wide_offsets()) {
        const auto* off =
            reinterpret_cast<const std::int64_t*>(c.offsets->data());
        begin = off[idx];
        end = off[idx + 1];
    } else {
        const auto* off =
            reinterpret_cast<const std::int32_t*>(c.offsets->data());
        begin = off[idx];
        end = off[idx + 1];
    }
    const std::int64_t len = end - begin;
    if (len > INLINE_MAX) {
        if (len > INT32_LIMIT)
            throw std::length_error("view column: value over INT32_MAX bytes");
        const char* bytes =
            reinterpret_cast<const char*>(c.data->data()) + begin;
        if (begin > INT32_LIMIT) {
            auto copy = Buffer::allocate(static_cast<std::size_t>(len));
            std::memcpy(copy->data(), bytes, static_cast<std::size_t>(len));
            const std::string_view sv(
                reinterpret_cast<const char*>(copy->data()),
                static_cast<std::size_t>(len));
            return append(sv, add_buffer(std::move(copy)));
        }
        return append(std::string_view(bytes, static_cast<std::size_t>(len)),
                      cached_buffer(c.data));
    }
    append_inline(std::string_view(
        len ? reinterpret_cast<const char*>(c.data->data()) + begin : "",
        static_cast<std::size_t>(len)));
}

void ViewBuilder::append_view_row(const dftu_series& c, std::int64_t row) {
    const std::uint8_t* src =
        c.data->data() + static_cast<std::size_t>(row) * VIEW_BYTES;
    std::uint8_t* v = new_view();
    std::memcpy(v, src, VIEW_BYTES);
    std::int32_t size;
    std::memcpy(&size, src, sizeof(size));
    if (size <= INLINE_MAX) return;
    std::int32_t index;
    std::memcpy(&index, src + 8, sizeof(index));
    const std::int32_t mapped =
        cached_buffer((*c.blobs)[static_cast<std::size_t>(index)]);
    std::memcpy(v + 8, &mapped, sizeof(mapped));
}

void ViewBuilder::append_row(const dftu_series& col, std::int64_t row) {
    const dftu_series* c = &col;
    std::int64_t i = row;
    for (;;) {
        if (own_null(*c, i)) return append_null();
        switch (c->encoding) {
            case Encoding::Flat:
                return append_flat(*c, i);
            case Encoding::View:
                return append_view_row(*c, i);
            case Encoding::Dictionary:
                i = reinterpret_cast<const std::int32_t*>(
                    c->data->data())[static_cast<std::size_t>(i)];
                c = c->child().get();
                break;
            case Encoding::Selection:
                i = reinterpret_cast<const std::int64_t*>(
                    c->data->data())[static_cast<std::size_t>(i)];
                if (i < 0) return append_null();
                c = c->child().get();
                break;
            case Encoding::Chunked: {
                std::int64_t local = 0;
                c = &c->chunk_at(i, local);
                i = local;
                break;
            }
            default:
                throw std::invalid_argument(
                    "view column: unsupported encoding");
        }
    }
}

void ViewBuilder::append_column(const dftu_series& col) {
    if (col.encoding == Encoding::Dictionary && col.child() &&
        col.child()->null_count == 0 && col.child()->length <= col.length) {
        const std::int64_t base = length();
        const std::size_t entries_at = views_.size();
        append_column(*col.child());
        const std::size_t entries = views_.size() - entries_at;
        const std::vector<std::uint8_t> entry_views(
            views_.begin() + static_cast<std::ptrdiff_t>(entries_at),
            views_.end());
        views_.resize(entries_at +
                      static_cast<std::size_t>(col.length) * VIEW_BYTES);
        std::uint8_t* dst = views_.data() + entries_at;
        const auto* codes =
            reinterpret_cast<const std::int32_t*>(col.data->data());
        const std::size_t k = entries / VIEW_BYTES;
        for (std::int64_t i = 0; i < col.length; ++i) {
            const auto code = static_cast<std::uint32_t>(codes[i]);
            if (code < k)
                std::memcpy(dst + static_cast<std::size_t>(i) * VIEW_BYTES,
                            entry_views.data() + code * VIEW_BYTES, VIEW_BYTES);
            else
                std::memset(dst + static_cast<std::size_t>(i) * VIEW_BYTES, 0,
                            VIEW_BYTES);
            if (code >= k || own_null(col, i)) null_rows_.push_back(base + i);
        }
        return;
    }
    if (col.encoding != Encoding::View) {
        for (std::int64_t i = 0; i < col.length; ++i) append_row(col, i);
        return;
    }
    const std::int64_t n = col.length;
    if (n == 0) return;
    std::vector<std::int32_t> remap;
    if (col.blobs) {
        remap.reserve(col.blobs->size());
        for (const auto& b : *col.blobs) remap.push_back(add_buffer(b));
    }
    const std::int64_t base = length();
    const std::size_t at = views_.size();
    views_.resize(at + static_cast<std::size_t>(n) * VIEW_BYTES);
    std::uint8_t* dst = views_.data() + at;
    const std::uint8_t* src = col.data->data();
    std::memcpy(dst, src, static_cast<std::size_t>(n) * VIEW_BYTES);
    for (std::int64_t i = 0; i < n; ++i) {
        std::uint8_t* v = dst + static_cast<std::size_t>(i) * VIEW_BYTES;
        std::int32_t size;
        std::memcpy(&size, v, sizeof(size));
        if (size <= INLINE_MAX) continue;
        std::int32_t index;
        std::memcpy(&index, v + 8, sizeof(index));
        const std::int32_t mapped = remap[static_cast<std::size_t>(index)];
        std::memcpy(v + 8, &mapped, sizeof(mapped));
    }
    if (col.validity && col.null_count > 0)
        for (std::int64_t i = 0; i < n; ++i)
            if (own_null(col, i)) null_rows_.push_back(base + i);
}

Series ViewBuilder::finish(TypeId type, bool json) {
    const std::int64_t n = length();
    auto* out = new dftu_series();
    out->type = type;
    out->set_json(json);
    out->encoding = Encoding::View;
    out->length = n;
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(std::move(views_));
    out->data = Buffer::wrap(bytes->data(), bytes->size(), [bytes](void*) {});
    out->blobs = std::make_shared<const std::vector<std::shared_ptr<Buffer>>>(
        std::move(buffers_));
    if (!null_rows_.empty()) {
        out->validity = Buffer::allocate(static_cast<std::size_t>((n + 7) / 8));
        std::memset(out->validity->data(), 0xFF, out->validity->size());
        for (std::int64_t r : null_rows_)
            out->validity->data()[r >> 3] &=
                static_cast<std::uint8_t>(~(1u << (r & 7)));
        out->null_count = static_cast<std::int64_t>(null_rows_.size());
    }
    views_.clear();
    buffers_.clear();
    index_of_.clear();
    null_rows_.clear();
    last_data_ = nullptr;
    return Series{out};
}

}  // namespace dftracer::utils::dataframe
