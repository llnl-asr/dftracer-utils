// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/borrow_release_pin.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstring>

using dftracer::utils::dataframe::borrow_buffer;
using dftracer::utils::dataframe::buffer_bytes;
using dftracer::utils::dataframe::BufferView;
using dftracer::utils::dataframe::byte_width;
using dftracer::utils::dataframe::Encoding;
using dftracer::utils::dataframe::new_buffer;
using dftracer::utils::dataframe::TypeId;
using dftracer::utils::dataframe::writable;

namespace {

using dftracer::utils::dataframe::BorrowRelease;
using dftracer::utils::dataframe::drop_borrow_release_pin;
using dftracer::utils::dataframe::pin_borrow_release;

// Everything a deferred `release` needs: the callback, its context and the
// pin that keeps the library holding that code mapped until it has run. The
// pin is dropped after the call, never before, because dropping it can unmap
// the code.
struct BorrowOwner {
    BorrowRelease release = nullptr;
    void* ctx = nullptr;
    void* pin = nullptr;
};

void release_borrowed(void* owner) noexcept {
    dftracer::utils::dstd::UniquePtr<BorrowOwner> hold(
        static_cast<BorrowOwner*>(owner));
    if (hold->release != nullptr) hold->release(hold->ctx);
    drop_borrow_release_pin(hold->pin);
}

dftracer::utils::dstd::UniquePtr<BorrowOwner> make_borrow_owner(
    BorrowRelease release, void* ctx) {
    dftracer::utils::dstd::UniquePtr<BorrowOwner> hold =
        dftracer::utils::dstd::make_unique<BorrowOwner>();
    hold->release = release;
    hold->ctx = ctx;
    hold->pin = pin_borrow_release(release);
    return hold;
}

// One owner shared by the buffers of a borrowed column, so `release` runs once
// when the last of them dies rather than once per buffer.
dftracer::utils::dstd::SharedPtr<void> borrow_owner(BorrowRelease release,
                                                    void* ctx) {
    BorrowOwner* const held = make_borrow_owner(release, ctx).release();
    return dftracer::utils::dstd::SharedPtr<void>(
        nullptr, [held](void*) { release_borrowed(held); });
}

constexpr std::int64_t VIEW_BYTES = 16;
constexpr std::int32_t VIEW_INLINE_BYTES = 12;

std::int64_t validity_bytes(std::int64_t n) { return (n + 7) / 8; }

std::int64_t count_nulls(const std::uint8_t* validity, std::int64_t n) {
    if (validity == nullptr) return 0;
    std::int64_t nulls = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        if ((validity[i >> 3] & (1u << (i & 7))) == 0) ++nulls;
    }
    return nulls;
}

}  // namespace

dftu_series* dftu_series_new_flat(dftu_dtype type, const void* data,
                                  std::int64_t n,
                                  const std::uint8_t* validity) {
    TypeId t = static_cast<TypeId>(type);
    // Fixed-width only; String/Binary are offset+data and need their own
    // builder. FixedSizeBinary also has no builder here: this signature has
    // no fixed_size parameter to record on the result.
    if (!byte_width(t)) return nullptr;

    auto* col = new dftu_series();
    col->type = t;
    col->encoding = Encoding::Flat;
    col->length = n;

    std::size_t bytes = buffer_bytes(t, n);
    col->data = new_buffer(bytes);
    if (bytes != 0 && data != nullptr) {
        std::memcpy(writable(col->data), data, bytes);
    }

    if (validity != nullptr) {
        std::size_t vbytes = static_cast<std::size_t>(validity_bytes(n));
        col->validity = new_buffer(vbytes);
        std::memcpy(writable(col->validity), validity, vbytes);
        col->null_count = count_nulls(validity, n);
    }
    return col;
}

dftu_series* dftu_series_new_flat_borrowed(dftu_dtype type, const void* data,
                                           std::int64_t n,
                                           const std::uint8_t* validity,
                                           void (*release)(void* ctx),
                                           void* release_ctx) {
    TypeId t = static_cast<TypeId>(type);
    // Same fixed_size limitation as dftu_series_new_flat above.
    if (!byte_width(t)) {
        if (release != nullptr) release(release_ctx);
        return nullptr;
    }

    auto* col = new dftu_series();
    col->type = t;
    col->encoding = Encoding::Flat;
    col->length = n;

    std::size_t bytes = buffer_bytes(t, n);
    auto* base = static_cast<std::uint8_t*>(const_cast<void*>(data));
    if (validity == nullptr) {
        col->data = BufferView(dftracer::utils::memory::Block::borrow(
            base, bytes, release_borrowed,
            make_borrow_owner(release, release_ctx).release()));
        return col;
    }
    dftracer::utils::dstd::SharedPtr<void> owner =
        borrow_owner(release, release_ctx);
    col->data = borrow_buffer(base, bytes, owner);
    col->validity = borrow_buffer(
        validity, static_cast<std::size_t>(validity_bytes(n)), owner);
    col->null_count = count_nulls(validity, n);
    return col;
}

dftu_series* dftu_series_new_string_view(
    dftu_dtype type, const void* views, std::int64_t n,
    const std::uint8_t* validity, const void* const* buffers,
    const std::int64_t* sizes, std::int32_t n_buffers,
    void (*release)(void* ctx), void* ctx) {
    // Runs `release` once when the last column or buffer using it dies, and
    // on every early return below.
    dftracer::utils::dstd::SharedPtr<void> owner = borrow_owner(release, ctx);
    TypeId t = static_cast<TypeId>(type);
    if (t != TypeId::String && t != TypeId::Binary) return nullptr;
    if (n < 0 || n_buffers < 0) return nullptr;
    if ((n > 0 && views == nullptr) ||
        (n_buffers > 0 && (buffers == nullptr || sizes == nullptr)))
        return nullptr;
    for (std::int32_t k = 0; k < n_buffers; ++k)
        if (sizes[k] < 0) return nullptr;

    const auto* v = static_cast<const std::uint8_t*>(views);
    for (std::int64_t i = 0; i < n; ++i) {
        if (validity != nullptr && !((validity[i >> 3] >> (i & 7)) & 1))
            continue;
        std::int32_t size;
        std::memcpy(&size, v + i * VIEW_BYTES, sizeof(size));
        if (size < 0) return nullptr;
        if (size <= VIEW_INLINE_BYTES) continue;
        std::int32_t index, offset;
        std::memcpy(&index, v + i * VIEW_BYTES + 8, sizeof(index));
        std::memcpy(&offset, v + i * VIEW_BYTES + 12, sizeof(offset));
        if (index < 0 || index >= n_buffers || offset < 0 ||
            static_cast<std::int64_t>(offset) + size > sizes[index])
            return nullptr;
    }

    auto* col = new dftu_series();
    col->type = t;
    col->encoding = Encoding::View;
    col->length = n;
    col->data =
        borrow_buffer(v, static_cast<std::size_t>(n * VIEW_BYTES), owner);
    auto blobs = dftracer::utils::dstd::make_shared<
        dftracer::utils::dstd::Vector<BufferView>>();
    blobs->reserve(static_cast<std::size_t>(n_buffers));
    for (std::int32_t k = 0; k < n_buffers; ++k)
        blobs->push_back(borrow_buffer(
            static_cast<std::uint8_t*>(const_cast<void*>(buffers[k])),
            static_cast<std::size_t>(sizes[k]), owner));
    col->blobs = std::move(blobs);

    if (validity != nullptr) {
        // Borrowed under the same owner as the views and the data buffers.
        col->validity = borrow_buffer(
            validity, static_cast<std::size_t>(validity_bytes(n)), owner);
        col->null_count = count_nulls(validity, n);
    }
    return col;
}

dftu_series* dftu_series_new_string(dftu_dtype type,
                                    const std::int32_t* offsets,
                                    const void* data, std::int64_t n,
                                    const std::uint8_t* validity) {
    TypeId t = static_cast<TypeId>(type);
    if (t != TypeId::String && t != TypeId::Binary) return nullptr;
    if (offsets != nullptr && offsets[n] < 0) return nullptr;

    auto* col = new dftu_series();
    col->type = t;
    col->encoding = Encoding::Flat;
    col->length = n;

    std::size_t off_bytes =
        static_cast<std::size_t>(n + 1) * sizeof(std::int32_t);
    col->offsets = new_buffer(off_bytes);
    if (offsets != nullptr)
        std::memcpy(writable(col->offsets), offsets, off_bytes);
    std::int32_t data_size = offsets != nullptr ? offsets[n] : 0;

    col->data = new_buffer(static_cast<std::size_t>(data_size));
    if (data != nullptr && data_size > 0)
        std::memcpy(writable(col->data), data,
                    static_cast<std::size_t>(data_size));

    if (validity != nullptr) {
        std::size_t vbytes = static_cast<std::size_t>(validity_bytes(n));
        col->validity = new_buffer(vbytes);
        std::memcpy(writable(col->validity), validity, vbytes);
        col->null_count = count_nulls(validity, n);
    }
    return col;
}

dftu_series* dftu_series_new_string_borrowed(
    dftu_dtype type, const void* offsets, const void* data,
    std::int64_t data_size, std::int64_t n, const std::uint8_t* validity,
    void (*release)(void* ctx), void* ctx) {
    // Runs `release` once when the last column or buffer using it dies, and on
    // every early return below.
    dftracer::utils::dstd::SharedPtr<void> owner = borrow_owner(release, ctx);
    TypeId t = static_cast<TypeId>(type);
    const bool wide = t == TypeId::LargeString || t == TypeId::LargeBinary;
    if (!wide && t != TypeId::String && t != TypeId::Binary) return nullptr;
    if (n < 0 || data_size < 0) return nullptr;
    if (n > 0 && offsets == nullptr) return nullptr;
    // Arrow leaves the values buffer NULL for a column of all-empty rows.
    if (data == nullptr && data_size > 0) return nullptr;

    const auto* o32 = static_cast<const std::int32_t*>(offsets);
    const auto* o64 = static_cast<const std::int64_t*>(offsets);
    for (std::int64_t i = 0; i < n; ++i) {
        const std::int64_t lo = wide ? o64[i] : o32[i];
        const std::int64_t hi = wide ? o64[i + 1] : o32[i + 1];
        if (lo < 0 || hi < lo || hi > data_size) return nullptr;
    }

    auto* col = new dftu_series();
    col->type = t;
    col->encoding = Encoding::Flat;
    col->length = n;
    if (offsets != nullptr)
        col->offsets = borrow_buffer(
            offsets,
            static_cast<std::size_t>(n + 1) *
                (wide ? sizeof(std::int64_t) : sizeof(std::int32_t)),
            owner);
    if (data != nullptr)
        col->data =
            borrow_buffer(data, static_cast<std::size_t>(data_size), owner);
    if (validity != nullptr) {
        col->validity = borrow_buffer(
            validity, static_cast<std::size_t>(validity_bytes(n)), owner);
        col->null_count = count_nulls(validity, n);
    }
    return col;
}

dftu_series* dftu_series_new_struct(dftu_series** fields, const char** names,
                                    std::int32_t n_fields) {
    if (n_fields < 0 || (n_fields > 0 && fields == nullptr)) return nullptr;
    auto* col = new dftu_series();
    col->type = TypeId::Struct;
    col->encoding = Encoding::Flat;
    // An empty batch is a valid 0-field, 0-row struct, not an error; returning
    // null here made an empty result crash the Arrow export (null handle
    // deref).
    col->length = n_fields > 0 ? fields[0]->length : 0;
    for (std::int32_t i = 0; i < n_fields; ++i) {
        col->nested.push_back(
            {adopt_series(fields[i]), names && names[i] ? names[i] : ""});
    }
    return col;
}

dftu_series* dftu_series_new_list(const std::int32_t* offsets, std::int64_t n,
                                  dftu_series* values) {
    auto* col = new dftu_series();
    col->type = TypeId::List;
    col->encoding = Encoding::Flat;
    col->length = n;
    std::size_t off_bytes =
        static_cast<std::size_t>(n + 1) * sizeof(std::int32_t);
    col->offsets = new_buffer(off_bytes);
    if (offsets != nullptr)
        std::memcpy(writable(col->offsets), offsets, off_bytes);
    col->set_child(adopt_series(values));
    return col;
}

void dftu_series_free(dftu_series* col) { delete col; }

std::int32_t dftu_series_type(const dftu_series* col) {
    return static_cast<std::int32_t>(col->type);
}

std::int32_t dftu_series_encoding(const dftu_series* col) {
    return static_cast<std::int32_t>(col->encoding);
}

std::int64_t dftu_series_length(const dftu_series* col) { return col->length; }

std::int64_t dftu_series_null_count(const dftu_series* col) {
    // A SELECTION view holds no count of its own; its nulls are the base's
    // at the selected rows plus the outer-fill sentinels.
    if (col->encoding == dftracer::utils::dataframe::Encoding::Selection &&
        col->child()) {
        std::int64_t n = 0;
        for (std::int64_t i = 0; i < col->length; ++i)
            n += dftu_series_is_null(col, i);
        return n;
    }
    return col->null_count;
}

const void* dftu_series_data(const dftu_series* col) {
    if (col->encoding != Encoding::Flat || !col->data.block()) return nullptr;
    return col->data.data();
}

const std::int32_t* dftu_series_offsets(const dftu_series* col) {
    if (!col->offsets.block() || col->wide_offsets()) return nullptr;
    return reinterpret_cast<const std::int32_t*>(col->offsets.data());
}

const std::int64_t* dftu_series_offsets64(const dftu_series* col) {
    if (!col->offsets.block() || !col->wide_offsets()) return nullptr;
    return reinterpret_cast<const std::int64_t*>(col->offsets.data());
}

const std::uint8_t* dftu_series_validity(const dftu_series* col,
                                         std::int64_t* bit_offset) {
    // No storage keeps a bit offset: a slice repacks the bitmap from bit 0.
    if (bit_offset != nullptr) *bit_offset = 0;
    if (col == nullptr) return nullptr;
    if (col->encoding != Encoding::Flat && col->encoding != Encoding::View)
        return nullptr;
    if (!col->validity.block()) return nullptr;
    return col->validity.data();
}

std::int32_t dftu_series_chunk_count(const dftu_series* col) {
    if (col == nullptr) return 0;
    if (!col->is_chunked()) return 1;
    return static_cast<std::int32_t>(col->nested.size());
}

dftu_series* dftu_series_chunk(const dftu_series* col, std::int32_t i) {
    if (col == nullptr || i < 0) return nullptr;
    if (!col->is_chunked()) return i == 0 ? new dftu_series(*col) : nullptr;
    const auto at = static_cast<std::size_t>(i);
    if (at >= col->nested.size() || !col->nested[at].series) return nullptr;
    return new dftu_series(*col->nested[at].series);
}

dftu_series* dftu_series_dictionary_codes(const dftu_series* col) {
    if (col == nullptr || col->encoding != Encoding::Dictionary ||
        !col->data.block())
        return nullptr;
    auto* out = new dftu_series();
    out->type = TypeId::Int32;
    out->encoding = Encoding::Flat;
    out->length = col->length;
    out->data = col->data;
    out->validity = col->validity;
    out->null_count = col->null_count;
    return out;
}

dftu_series* dftu_series_dictionary_values(const dftu_series* col) {
    if (col == nullptr || col->encoding != Encoding::Dictionary) return nullptr;
    const auto& values = col->child();
    if (!values) return nullptr;
    return new dftu_series(*values);
}

std::int32_t dftu_series_view_buffer_count(const dftu_series* col) {
    if (col == nullptr || col->encoding != Encoding::View) return -1;
    return col->blobs ? static_cast<std::int32_t>(col->blobs->size()) : 0;
}

const void* dftu_series_view_data(const dftu_series* col) {
    if (col == nullptr || col->encoding != Encoding::View || !col->data.block())
        return nullptr;
    return col->data.data();
}

const void* dftu_series_view_buffer(const dftu_series* col, std::int32_t i,
                                    std::int64_t* size) {
    if (size != nullptr) *size = 0;
    if (i < 0 || dftu_series_view_buffer_count(col) <= i) return nullptr;
    const BufferView& blob = (*col->blobs)[static_cast<std::size_t>(i)];
    if (size != nullptr) *size = static_cast<std::int64_t>(blob.size());
    return blob.block() ? blob.data() : nullptr;
}

std::int32_t dftu_series_is_null(const dftu_series* col, std::int64_t i) {
    if (col->is_chunked()) {
        std::int64_t local = 0;
        const dftu_series& c = col->chunk_at(i, local);
        return dftu_series_is_null(&c, local);
    }
    // A SELECTION row is null when its index is the outer-fill sentinel, its
    // own bitmap (set by an Arrow import) says so, or the base row is null.
    if (col->encoding == dftracer::utils::dataframe::Encoding::Selection &&
        col->child()) {
        const std::int64_t idx =
            reinterpret_cast<const std::int64_t*>(col->data.data())[i];
        if (idx < 0) return 1;
        if (col->validity.block() &&
            !((col->validity.data()[i >> 3] >> (i & 7)) & 1))
            return 1;
        return dftu_series_is_null(col->child().get(), idx);
    }
    if (col->validity.block() &&
        !((col->validity.data()[i >> 3] >> (i & 7)) & 1))
        return 1;
    // A dictionary entry may itself be null (an Arrow import can carry one).
    if (col->encoding == dftracer::utils::dataframe::Encoding::Dictionary &&
        col->child() && col->child()->null_count > 0)
        return dftu_series_is_null(
            col->child().get(),
            reinterpret_cast<const std::int32_t*>(col->data.data())[i]);
    return 0;
}

const char* dftu_series_string_at(const dftu_series* col, std::int64_t i,
                                  std::int64_t* len) {
    *len = 0;
    if (col == nullptr) return nullptr;
    switch (col->type) {
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
            break;
        default:
            return nullptr;
    }
    while (true) {
        if (i < 0 || i >= col->length) return nullptr;
        switch (col->encoding) {
            case Encoding::Flat: {
                if (dftu_series_is_null(col, i) || !col->data.block() ||
                    !col->offsets.block())
                    return nullptr;
                const char* d = reinterpret_cast<const char*>(col->data.data());
                if (col->wide_offsets()) {
                    const auto* off = reinterpret_cast<const std::int64_t*>(
                        col->offsets.data());
                    *len = off[i + 1] - off[i];
                    return d + off[i];
                }
                const auto* off =
                    reinterpret_cast<const std::int32_t*>(col->offsets.data());
                *len = off[i + 1] - off[i];
                return d + off[i];
            }
            case Encoding::Dictionary: {
                if (dftu_series_is_null(col, i) || !col->child() ||
                    !col->data.block())
                    return nullptr;
                i = reinterpret_cast<const std::int32_t*>(col->data.data())[i];
                col = col->child().get();
                break;
            }
            case Encoding::View: {
                if (dftu_series_is_null(col, i) || !col->data.block())
                    return nullptr;
                const std::uint8_t* view = col->data.data() + i * VIEW_BYTES;
                std::int32_t size;
                std::memcpy(&size, view, sizeof(size));
                *len = size;
                if (size <= VIEW_INLINE_BYTES)
                    return reinterpret_cast<const char*>(view + 4);
                std::int32_t index, offset;
                std::memcpy(&index, view + 8, sizeof(index));
                std::memcpy(&offset, view + 12, sizeof(offset));
                if (!col->blobs ||
                    static_cast<std::size_t>(index) >= col->blobs->size()) {
                    *len = 0;
                    return nullptr;
                }
                return reinterpret_cast<const char*>(
                    (*col->blobs)[static_cast<std::size_t>(index)].data() +
                    offset);
            }
            case Encoding::Selection: {
                if (!col->child() || !col->data.block()) return nullptr;
                const std::int64_t idx =
                    reinterpret_cast<const std::int64_t*>(col->data.data())[i];
                if (idx < 0) return nullptr;
                i = idx;
                col = col->child().get();
                break;
            }
            case Encoding::Chunked: {
                std::int64_t local = 0;
                col = &col->chunk_at(i, local);
                i = local;
                break;
            }
            default:
                return nullptr;
        }
    }
}

namespace {
void buffer_bytes_once(const dftu_series* col,
                       dftracer::utils::dstd::HashSet<const void*>& seen,
                       std::int64_t& total) {
    auto add = [&](const auto& b) {
        if (b.block() && seen.insert(b.data()).second)
            total += static_cast<std::int64_t>(b.size());
    };
    add(col->data);
    add(col->offsets);
    add(col->validity);
    if (col->blobs)
        for (const auto& b : *col->blobs) add(b);
    for (const auto& n : col->nested)
        if (n.series) buffer_bytes_once(n.series.get(), seen, total);
}
}  // namespace

std::int64_t dftu_series_buffer_bytes(const dftu_series* col) {
    if (col == nullptr) return 0;
    std::int64_t total = 0;
    if (col->is_chunked()) {
        dftracer::utils::dstd::HashSet<const void*> seen;
        buffer_bytes_once(col, seen, total);
        return total;
    }
    if (col->data.block()) total += static_cast<std::int64_t>(col->data.size());
    if (col->offsets.block())
        total += static_cast<std::int64_t>(col->offsets.size());
    if (col->validity.block())
        total += static_cast<std::int64_t>(col->validity.size());
    if (col->blobs)
        for (const auto& b : *col->blobs)
            total += static_cast<std::int64_t>(b.size());
    for (const auto& n : col->nested)
        if (n.series) total += dftu_series_buffer_bytes(n.series.get());
    return total;
}

namespace {
bool is_single_child_container(TypeId t) {
    return t == TypeId::List || t == TypeId::LargeList ||
           t == TypeId::FixedSizeList || t == TypeId::Map;
}
}  // namespace

std::int32_t dftu_series_num_children(const dftu_series* col) {
    if (is_single_child_container(col->type)) return col->child() ? 1 : 0;
    return static_cast<std::int32_t>(col->num_fields());
}

dftu_series* dftu_series_child(const dftu_series* col, std::int32_t i) {
    const dftracer::utils::dstd::SharedPtr<dftu_series>* ch = nullptr;
    if (is_single_child_container(col->type)) {
        if (i == 0 && col->child()) ch = &col->child();
    } else if (i >= 0 && static_cast<std::size_t>(i) < col->num_fields()) {
        ch = &col->nested[static_cast<std::size_t>(i)].series;
    }
    if (!ch || !*ch) return nullptr;
    // Owned copy sharing the child's buffers (shared_ptr members).
    return new dftu_series(**ch);
}

const char* dftu_series_field_name(const dftu_series* col, std::int32_t i) {
    if (!col || col->type != TypeId::Struct) return nullptr;
    if (i < 0 || static_cast<std::size_t>(i) >= col->num_fields())
        return nullptr;
    return col->nested[static_cast<std::size_t>(i)].name.c_str();
}

std::int32_t dftu_series_time_unit(const dftu_series* col) {
    return col ? static_cast<std::int32_t>(col->time_unit())
               : static_cast<std::int32_t>(
                     dftracer::utils::dataframe::TimeUnit::Micro);
}

const char* dftu_series_timezone(const dftu_series* col) {
    static const char empty[] = "";
    // A zone name is interned, so its text is terminated and never freed.
    return col && !col->timezone().empty() ? col->timezone().data() : empty;
}

std::int32_t dftu_series_decimal_precision(const dftu_series* col) {
    return col ? col->decimal_precision() : 0;
}

std::int32_t dftu_series_decimal_scale(const dftu_series* col) {
    return col ? col->decimal_scale() : 0;
}

std::int32_t dftu_series_fixed_size(const dftu_series* col) {
    return col ? col->fixed_size() : 0;
}

std::int32_t dftu_series_is_json(const dftu_series* col) {
    return col && col->json() ? 1 : 0;
}

dftu_series* dftu_series_mark_json(const dftu_series* col) {
    if (!col || col->type != TypeId::String) return nullptr;
    auto* out = new dftu_series(*col);
    out->set_json(true);
    return out;
}

dftu_series* dftu_series_share(const dftu_series* col) {
    if (!col) return nullptr;
    // Copy the handle struct; its buffer/child shared_ptr members bump their
    // refcounts, so the new column shares the same data with no copy.
    return new dftu_series(*col);
}

const char* dftu_series_name(const dftu_series* s, std::size_t* len) {
    if (len) *len = s ? s->name.size() : 0;
    return s && !s->name.empty() ? s->name.data() : nullptr;
}

dftu_series* dftu_series_rename(const dftu_series* s, const char* name,
                                std::size_t len) {
    if (!s) return nullptr;
    auto* out = new dftu_series(*s);
    if (name && len > 0)
        out->name = dftracer::utils::dstd::SharedString{
            dftracer::utils::dstd::StringView{name, len}};
    else
        out->name = {};
    return out;
}

std::int32_t dftu_series_index_count(const dftu_series* s) {
    return s && s->labels ? static_cast<std::int32_t>(s->labels->levels.size())
                          : 0;
}

std::int32_t dftu_series_index_name(const dftu_series* s, std::int32_t i,
                                    const char** ptr, std::size_t* len) {
    if (!s || !ptr || i < 0 || !s->labels ||
        static_cast<std::size_t>(i) >= s->labels->levels.size())
        return -1;
    const auto& name = s->labels->levels[static_cast<std::size_t>(i)].name;
    *ptr = name.c_str();
    if (len) *len = name.size();
    return 0;
}

dftu_series* dftu_series_index_column(const dftu_series* s, std::int32_t i) {
    if (!s || i < 0 || !s->labels ||
        static_cast<std::size_t>(i) >= s->labels->levels.size())
        return nullptr;
    return dftu_series_share(
        s->labels->levels[static_cast<std::size_t>(i)].series.get());
}

dftu_series* dftu_series_with_index(const dftu_series* s,
                                    const char* const* names,
                                    const std::size_t* lens,
                                    const dftu_series* const* levels,
                                    std::int32_t n) {
    if (!s || n < 0 || (n > 0 && (!names || !lens || !levels))) return nullptr;
    auto labels = dftracer::utils::dstd::make_shared<dftu_labels>();
    labels->levels.reserve(static_cast<std::size_t>(n));
    for (std::int32_t i = 0; i < n; ++i) {
        const dftu_series* level = levels[i];
        if (!names[i] || !level || level->length != s->length ||
            level->type == TypeId::List || level->type == TypeId::Struct)
            return nullptr;
        const dftracer::utils::dstd::StringView name(names[i], lens[i]);
        for (const dftu_nested& seen : labels->levels)
            if (dftracer::utils::dstd::StringView(seen.name) == name)
                return nullptr;
        dftu_series* shared = dftu_series_share(level);
        shared->labels = {};
        shared->name = {};
        labels->levels.push_back(
            {adopt_series(shared), dftracer::utils::dstd::String(name)});
    }
    auto* out = new dftu_series(*s);
    if (n == 0)
        out->labels = {};
    else
        out->labels = std::move(labels);
    return out;
}

namespace {

BufferView window(const BufferView& parent, std::size_t at, std::size_t bytes) {
    return parent.slice(at, bytes);
}

// Bits [offset, offset+len) of `bits` from bit 0: shared when the offset is
// on a byte boundary (and the end too when `exact`, for Bool values that
// byte-wise kernels count), repacked otherwise.
BufferView window_bits(const BufferView& bits, std::int64_t offset,
                       std::int64_t len, bool exact) {
    const std::size_t nbytes = static_cast<std::size_t>((len + 7) / 8);
    if (len == 0) return new_buffer(0);
    if ((offset & 7) == 0 && (!exact || (len & 7) == 0))
        return window(bits, static_cast<std::size_t>(offset >> 3), nbytes);
    auto out = new_buffer(nbytes);
    const std::uint8_t* src = bits.data() + (offset >> 3);
    const unsigned r = static_cast<unsigned>(offset & 7);
    // The last source byte holding bit offset+len-1; reading past it would
    // leave the parent buffer.
    const std::size_t last =
        static_cast<std::size_t>(((offset & 7) + len - 1) >> 3);
    std::uint8_t* dst = writable(out);
    for (std::size_t k = 0; k < nbytes; ++k) {
        const unsigned hi = k + 1 <= last ? src[k + 1] : 0u;
        dst[k] = static_cast<std::uint8_t>((src[k] >> r) | (hi << (8 - r)));
    }
    if (len & 7)
        dst[nbytes - 1] &= static_cast<std::uint8_t>((1u << (len & 7)) - 1u);
    return out;
}

template <class Off>
BufferView rebased_offsets(const dftu_series& col, std::int64_t offset,
                           std::int64_t len) {
    const Off* src = reinterpret_cast<const Off*>(col.offsets.data()) + offset;
    auto out = new_buffer(static_cast<std::size_t>(len + 1) * sizeof(Off));
    Off* dst = reinterpret_cast<Off*>(writable(out));
    for (std::int64_t i = 0; i <= len; ++i) dst[i] = src[i] - src[0];
    return out;
}

dftu_series* slice_unnamed(const dftu_series* col, std::int64_t offset,
                           std::int64_t len) {
    if (!col) return nullptr;
    if (offset < 0) offset = 0;
    if (offset > col->length) offset = col->length;
    if (len < 0 || len > col->length - offset) len = col->length - offset;
    if (col->is_chunked()) {
        const auto* starts =
            reinterpret_cast<const std::int64_t*>(col->data.data());
        dftracer::utils::dstd::Vector<
            dftracer::utils::dstd::SharedPtr<dftu_series>>
            parts;
        for (std::size_t k = 0; k < col->nested.size(); ++k) {
            const std::int64_t lo = std::max(offset, starts[k]);
            const std::int64_t hi = std::min(offset + len, starts[k + 1]);
            if (hi <= lo) continue;
            dftu_series* part = slice_unnamed(col->nested[k].series.get(),
                                              lo - starts[k], hi - lo);
            if (!part) return nullptr;
            parts.push_back(adopt_series(part));
        }
        if (parts.empty())
            return slice_unnamed(col->nested.front().series.get(), 0, 0);
        if (parts.size() == 1) return new dftu_series(*parts.front());
        return dftracer::utils::dataframe::make_chunked(std::move(parts));
    }
    const std::size_t w = byte_width(col->type, col->fixed_size()).value_or(0);
    const bool text =
        col->type == TypeId::String || col->type == TypeId::Binary ||
        col->type == TypeId::LargeString || col->type == TypeId::LargeBinary;
    std::size_t index_width = 0;
    switch (col->encoding) {
        case Encoding::Selection:
            index_width = sizeof(std::int64_t);
            break;
        case Encoding::Dictionary:
            index_width = sizeof(std::int32_t);
            break;
        case Encoding::View:
            index_width = 16;
            break;
        case Encoding::Flat:
            if (text ? !col->offsets.block() : (w == 0 || !col->data.block()))
                return nullptr;
            break;
        default:
            return nullptr;
    }
    if (index_width != 0 && !col->data.block()) return nullptr;

    auto* out = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*out, *col);
    out->encoding = col->encoding;
    out->length = len;
    out->nested = col->nested;
    out->blobs = col->blobs;
    if (index_width != 0) {
        out->data =
            window(col->data, static_cast<std::size_t>(offset) * index_width,
                   static_cast<std::size_t>(len) * index_width);
    } else if (col->type == TypeId::Bool) {
        out->data = window_bits(col->data, offset, len, true);
    } else if (text) {
        const bool wide = col->wide_offsets();
        std::int64_t lo = 0, hi = 0;
        if (wide) {
            const auto* o =
                reinterpret_cast<const std::int64_t*>(col->offsets.data());
            lo = o[offset];
            hi = o[offset + len];
            out->offsets = rebased_offsets<std::int64_t>(*col, offset, len);
        } else {
            const auto* o =
                reinterpret_cast<const std::int32_t*>(col->offsets.data());
            lo = o[offset];
            hi = o[offset + len];
            out->offsets = rebased_offsets<std::int32_t>(*col, offset, len);
        }
        out->data = col->data.block()
                        ? window(col->data, static_cast<std::size_t>(lo),
                                 static_cast<std::size_t>(hi - lo))
                        : BufferView{};
    } else {
        out->data = window(col->data, static_cast<std::size_t>(offset) * w,
                           static_cast<std::size_t>(len) * w);
    }
    if (col->validity.block() && len > 0) {
        out->validity = window_bits(col->validity, offset, len, false);
        out->null_count = count_nulls(out->validity.data(), len);
    }
    return out;
}

}  // namespace

dftu_series* dftu_series_slice(const dftu_series* col, std::int64_t offset,
                               std::int64_t len) {
    dftu_series* out = slice_unnamed(col, offset, len);
    if (!out) return out;
    out->name = col->name;
    if (!col->labels) return out;
    auto labels = dftracer::utils::dstd::make_shared<dftu_labels>();
    for (const dftu_nested& level : col->labels->levels) {
        dftu_series* part = slice_unnamed(level.series.get(), offset, len);
        if (!part) {
            dftu_series_free(out);
            return nullptr;
        }
        labels->levels.push_back({adopt_series(part), level.name});
    }
    out->labels = std::move(labels);
    return out;
}
