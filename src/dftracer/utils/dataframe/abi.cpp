#include <ankerl/unordered_dense.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>

#include <cstring>

using dftracer::utils::dataframe::Buffer;
using dftracer::utils::dataframe::buffer_bytes;
using dftracer::utils::dataframe::byte_width;
using dftracer::utils::dataframe::Encoding;
using dftracer::utils::dataframe::TypeId;

namespace {

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

dftu_series* dftu_series_new_flat(dftu_dtype type, const void* data, int64_t n,
                                  const uint8_t* validity) {
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
    col->data = Buffer::allocate(bytes);
    if (bytes != 0 && data != nullptr) {
        std::memcpy(col->data->data(), data, bytes);
    }

    if (validity != nullptr) {
        std::size_t vbytes = static_cast<std::size_t>(validity_bytes(n));
        col->validity = Buffer::allocate(vbytes);
        std::memcpy(col->validity->data(), validity, vbytes);
        col->null_count = count_nulls(validity, n);
    }
    return col;
}

dftu_series* dftu_series_new_flat_borrowed(dftu_dtype type, const void* data,
                                           int64_t n, const uint8_t* validity,
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
    col->data = Buffer::wrap(base, bytes, [release, release_ctx](void*) {
        if (release != nullptr) release(release_ctx);
    });

    if (validity != nullptr) {
        std::size_t vbytes = static_cast<std::size_t>(validity_bytes(n));
        col->validity = Buffer::allocate(vbytes);
        std::memcpy(col->validity->data(), validity, vbytes);
        col->null_count = count_nulls(validity, n);
    }
    return col;
}

dftu_series* dftu_series_new_string_view(
    dftu_dtype type, const void* views, int64_t n, const uint8_t* validity,
    const void* const* buffers, const int64_t* sizes, int32_t n_buffers,
    void (*release)(void* ctx), void* ctx) {
    // Runs `release` once when the last column or buffer using it dies, and
    // on every early return below.
    std::shared_ptr<void> owner(nullptr, [release, ctx](void*) {
        if (release != nullptr) release(ctx);
    });
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
    col->data = Buffer::wrap(const_cast<std::uint8_t*>(v),
                             static_cast<std::size_t>(n * VIEW_BYTES),
                             [owner](void*) {});
    auto blobs = std::make_shared<std::vector<std::shared_ptr<Buffer>>>();
    blobs->reserve(static_cast<std::size_t>(n_buffers));
    for (std::int32_t k = 0; k < n_buffers; ++k)
        blobs->push_back(Buffer::wrap(
            static_cast<std::uint8_t*>(const_cast<void*>(buffers[k])),
            static_cast<std::size_t>(sizes[k]), [owner](void*) {}));
    col->blobs = std::move(blobs);

    if (validity != nullptr) {
        std::size_t vbytes = static_cast<std::size_t>(validity_bytes(n));
        col->validity = Buffer::allocate(vbytes);
        std::memcpy(col->validity->data(), validity, vbytes);
        col->null_count = count_nulls(validity, n);
    }
    return col;
}

dftu_series* dftu_series_new_string(dftu_dtype type, const int32_t* offsets,
                                    const void* data, int64_t n,
                                    const uint8_t* validity) {
    TypeId t = static_cast<TypeId>(type);
    if (t != TypeId::String && t != TypeId::Binary) return nullptr;
    if (offsets != nullptr && offsets[n] < 0) return nullptr;

    auto* col = new dftu_series();
    col->type = t;
    col->encoding = Encoding::Flat;
    col->length = n;

    std::size_t off_bytes =
        static_cast<std::size_t>(n + 1) * sizeof(std::int32_t);
    col->offsets = Buffer::allocate(off_bytes);
    if (offsets != nullptr)
        std::memcpy(col->offsets->data(), offsets, off_bytes);
    std::int32_t data_len = offsets != nullptr ? offsets[n] : 0;

    col->data = Buffer::allocate(static_cast<std::size_t>(data_len));
    if (data != nullptr && data_len > 0)
        std::memcpy(col->data->data(), data,
                    static_cast<std::size_t>(data_len));

    if (validity != nullptr) {
        std::size_t vbytes = static_cast<std::size_t>(validity_bytes(n));
        col->validity = Buffer::allocate(vbytes);
        std::memcpy(col->validity->data(), validity, vbytes);
        col->null_count = count_nulls(validity, n);
    }
    return col;
}

dftu_series* dftu_series_new_struct(dftu_series** fields, const char** names,
                                    int32_t n_fields) {
    if (n_fields < 0 || (n_fields > 0 && fields == nullptr)) return nullptr;
    auto* col = new dftu_series();
    col->type = TypeId::Struct;
    col->encoding = Encoding::Flat;
    // An empty batch is a valid 0-field, 0-row struct, not an error; returning
    // null here made an empty result crash the Arrow export (null handle
    // deref).
    col->length = n_fields > 0 ? fields[0]->length : 0;
    for (int32_t i = 0; i < n_fields; ++i) {
        col->nested.push_back({std::shared_ptr<dftu_series>(fields[i]),
                               names && names[i] ? names[i] : ""});
    }
    return col;
}

dftu_series* dftu_series_new_list(const int32_t* offsets, int64_t n,
                                  dftu_series* values) {
    auto* col = new dftu_series();
    col->type = TypeId::List;
    col->encoding = Encoding::Flat;
    col->length = n;
    std::size_t off_bytes =
        static_cast<std::size_t>(n + 1) * sizeof(std::int32_t);
    col->offsets = Buffer::allocate(off_bytes);
    if (offsets != nullptr)
        std::memcpy(col->offsets->data(), offsets, off_bytes);
    col->set_child(std::shared_ptr<dftu_series>(values));
    return col;
}

void dftu_series_free(dftu_series* col) { delete col; }

int32_t dftu_series_type(const dftu_series* col) {
    return static_cast<int32_t>(col->type);
}

int32_t dftu_series_encoding(const dftu_series* col) {
    return static_cast<int32_t>(col->encoding);
}

int64_t dftu_series_length(const dftu_series* col) { return col->length; }

int64_t dftu_series_null_count(const dftu_series* col) {
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
    if (col->encoding != Encoding::Flat || !col->data) return nullptr;
    return col->data->data();
}

const int32_t* dftu_series_offsets(const dftu_series* col) {
    if (!col->offsets || col->wide_offsets()) return nullptr;
    return reinterpret_cast<const int32_t*>(col->offsets->data());
}

const int64_t* dftu_series_offsets64(const dftu_series* col) {
    if (!col->offsets || !col->wide_offsets()) return nullptr;
    return reinterpret_cast<const int64_t*>(col->offsets->data());
}

int32_t dftu_series_is_null(const dftu_series* col, int64_t i) {
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
            reinterpret_cast<const std::int64_t*>(col->data->data())[i];
        if (idx < 0) return 1;
        if (col->validity && !((col->validity->data()[i >> 3] >> (i & 7)) & 1))
            return 1;
        return dftu_series_is_null(col->child().get(), idx);
    }
    if (col->validity && !((col->validity->data()[i >> 3] >> (i & 7)) & 1))
        return 1;
    // A dictionary entry may itself be null (an Arrow import can carry one).
    if (col->encoding == dftracer::utils::dataframe::Encoding::Dictionary &&
        col->child() && col->child()->null_count > 0)
        return dftu_series_is_null(
            col->child().get(),
            reinterpret_cast<const std::int32_t*>(col->data->data())[i]);
    return 0;
}

const char* dftu_series_string_at(const dftu_series* col, int64_t i,
                                  int64_t* len) {
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
                if (dftu_series_is_null(col, i) || !col->data || !col->offsets)
                    return nullptr;
                const char* d =
                    reinterpret_cast<const char*>(col->data->data());
                if (col->wide_offsets()) {
                    const auto* off = reinterpret_cast<const std::int64_t*>(
                        col->offsets->data());
                    *len = off[i + 1] - off[i];
                    return d + off[i];
                }
                const auto* off =
                    reinterpret_cast<const std::int32_t*>(col->offsets->data());
                *len = off[i + 1] - off[i];
                return d + off[i];
            }
            case Encoding::Dictionary: {
                if (dftu_series_is_null(col, i) || !col->child() || !col->data)
                    return nullptr;
                i = reinterpret_cast<const std::int32_t*>(col->data->data())[i];
                col = col->child().get();
                break;
            }
            case Encoding::View: {
                if (dftu_series_is_null(col, i) || !col->data) return nullptr;
                const std::uint8_t* view = col->data->data() + i * VIEW_BYTES;
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
                    (*col->blobs)[static_cast<std::size_t>(index)]->data() +
                    offset);
            }
            case Encoding::Selection: {
                if (!col->child() || !col->data) return nullptr;
                const std::int64_t idx =
                    reinterpret_cast<const std::int64_t*>(col->data->data())[i];
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
                       ankerl::unordered_dense::set<const void*>& seen,
                       std::int64_t& total) {
    auto add = [&](const auto& b) {
        if (b && seen.insert(b.get()).second)
            total += static_cast<std::int64_t>(b->size());
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

int64_t dftu_series_buffer_bytes(const dftu_series* col) {
    if (col == nullptr) return 0;
    std::int64_t total = 0;
    if (col->is_chunked()) {
        ankerl::unordered_dense::set<const void*> seen;
        buffer_bytes_once(col, seen, total);
        return total;
    }
    if (col->data) total += static_cast<std::int64_t>(col->data->size());
    if (col->offsets) total += static_cast<std::int64_t>(col->offsets->size());
    if (col->validity)
        total += static_cast<std::int64_t>(col->validity->size());
    if (col->blobs)
        for (const auto& b : *col->blobs)
            total += static_cast<std::int64_t>(b->size());
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

int32_t dftu_series_num_children(const dftu_series* col) {
    if (is_single_child_container(col->type)) return col->child() ? 1 : 0;
    return static_cast<int32_t>(col->num_fields());
}

dftu_series* dftu_series_child(const dftu_series* col, int32_t i) {
    const std::shared_ptr<dftu_series>* ch = nullptr;
    if (is_single_child_container(col->type)) {
        if (i == 0 && col->child()) ch = &col->child();
    } else if (i >= 0 && static_cast<std::size_t>(i) < col->num_fields()) {
        ch = &col->nested[static_cast<std::size_t>(i)].series;
    }
    if (!ch || !*ch) return nullptr;
    // Owned copy sharing the child's buffers (shared_ptr members).
    return new dftu_series(**ch);
}

const char* dftu_series_field_name(const dftu_series* col, int32_t i) {
    if (!col || col->type != TypeId::Struct) return nullptr;
    if (i < 0 || static_cast<std::size_t>(i) >= col->num_fields())
        return nullptr;
    return col->nested[static_cast<std::size_t>(i)].name.c_str();
}

int32_t dftu_series_time_unit(const dftu_series* col) {
    return col ? static_cast<int32_t>(col->time_unit())
               : static_cast<int32_t>(
                     dftracer::utils::dataframe::TimeUnit::Micro);
}

const char* dftu_series_timezone(const dftu_series* col) {
    static const char empty[] = "";
    // A zone name is interned, so its text is terminated and never freed.
    return col && !col->timezone().empty() ? col->timezone().data() : empty;
}

int32_t dftu_series_decimal_precision(const dftu_series* col) {
    return col ? col->decimal_precision() : 0;
}

int32_t dftu_series_decimal_scale(const dftu_series* col) {
    return col ? col->decimal_scale() : 0;
}

int32_t dftu_series_fixed_size(const dftu_series* col) {
    return col ? col->fixed_size() : 0;
}

int32_t dftu_series_is_json(const dftu_series* col) {
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

namespace {

// A window of `bytes` bytes at `at` inside `parent`, owned by `parent`.
std::shared_ptr<Buffer> window(const std::shared_ptr<Buffer>& parent,
                               std::size_t at, std::size_t bytes) {
    return Buffer::wrap(parent->data() + at, bytes,
                        [parent](void*) { /* view: parent owns it */ });
}

// Bits [offset, offset+len) of `bits` from bit 0: shared when the offset is
// on a byte boundary (and the end too when `exact`, for Bool values that
// byte-wise kernels count), repacked otherwise.
std::shared_ptr<Buffer> window_bits(const std::shared_ptr<Buffer>& bits,
                                    std::int64_t offset, std::int64_t len,
                                    bool exact) {
    const std::size_t nbytes = static_cast<std::size_t>((len + 7) / 8);
    if (len == 0) return Buffer::allocate(0);
    if ((offset & 7) == 0 && (!exact || (len & 7) == 0))
        return window(bits, static_cast<std::size_t>(offset >> 3), nbytes);
    auto out = Buffer::allocate(nbytes);
    const std::uint8_t* src = bits->data() + (offset >> 3);
    const unsigned r = static_cast<unsigned>(offset & 7);
    // The last source byte holding bit offset+len-1; reading past it would
    // leave the parent buffer.
    const std::size_t last =
        static_cast<std::size_t>(((offset & 7) + len - 1) >> 3);
    std::uint8_t* dst = out->data();
    for (std::size_t k = 0; k < nbytes; ++k) {
        const unsigned hi = k + 1 <= last ? src[k + 1] : 0u;
        dst[k] = static_cast<std::uint8_t>((src[k] >> r) | (hi << (8 - r)));
    }
    if (len & 7)
        dst[nbytes - 1] &= static_cast<std::uint8_t>((1u << (len & 7)) - 1u);
    return out;
}

template <class Off>
std::shared_ptr<Buffer> rebased_offsets(const dftu_series& col,
                                        std::int64_t offset, std::int64_t len) {
    const Off* src = reinterpret_cast<const Off*>(col.offsets->data()) + offset;
    auto out =
        Buffer::allocate(static_cast<std::size_t>(len + 1) * sizeof(Off));
    Off* dst = reinterpret_cast<Off*>(out->data());
    for (std::int64_t i = 0; i <= len; ++i) dst[i] = src[i] - src[0];
    return out;
}

}  // namespace

dftu_series* dftu_series_slice(const dftu_series* col, int64_t offset,
                               int64_t len) {
    if (!col) return nullptr;
    if (offset < 0) offset = 0;
    if (offset > col->length) offset = col->length;
    if (len < 0 || len > col->length - offset) len = col->length - offset;
    if (col->is_chunked()) {
        const auto* starts =
            reinterpret_cast<const std::int64_t*>(col->data->data());
        std::vector<std::shared_ptr<dftu_series>> parts;
        for (std::size_t k = 0; k < col->nested.size(); ++k) {
            const std::int64_t lo = std::max(offset, starts[k]);
            const std::int64_t hi = std::min(offset + len, starts[k + 1]);
            if (hi <= lo) continue;
            dftu_series* part = dftu_series_slice(col->nested[k].series.get(),
                                                  lo - starts[k], hi - lo);
            if (!part) return nullptr;
            parts.emplace_back(part);
        }
        if (parts.empty())
            return dftu_series_slice(col->nested.front().series.get(), 0, 0);
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
            if (text ? !col->offsets : (w == 0 || !col->data)) return nullptr;
            break;
        default:
            return nullptr;
    }
    if (index_width != 0 && !col->data) return nullptr;

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
                reinterpret_cast<const std::int64_t*>(col->offsets->data());
            lo = o[offset];
            hi = o[offset + len];
            out->offsets = rebased_offsets<std::int64_t>(*col, offset, len);
        } else {
            const auto* o =
                reinterpret_cast<const std::int32_t*>(col->offsets->data());
            lo = o[offset];
            hi = o[offset + len];
            out->offsets = rebased_offsets<std::int32_t>(*col, offset, len);
        }
        out->data = col->data ? window(col->data, static_cast<std::size_t>(lo),
                                       static_cast<std::size_t>(hi - lo))
                              : nullptr;
    } else {
        out->data = window(col->data, static_cast<std::size_t>(offset) * w,
                           static_cast<std::size_t>(len) * w);
    }
    if (col->validity && len > 0) {
        out->validity = window_bits(col->validity, offset, len, false);
        out->null_count = count_nulls(out->validity->data(), len);
    }
    return out;
}
