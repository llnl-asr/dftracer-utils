#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>

#include <cstring>

using dftracer::utils::dataframe::Buffer;
using dftracer::utils::dataframe::buffer_bytes;
using dftracer::utils::dataframe::byte_width;
using dftracer::utils::dataframe::Encoding;
using dftracer::utils::dataframe::TypeId;

namespace {

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

dftu_series* dftu_series_new_string(dftu_dtype type, const int32_t* offsets,
                                    const void* data, int64_t n,
                                    const uint8_t* validity) {
    TypeId t = static_cast<TypeId>(type);
    if (t != TypeId::String && t != TypeId::Binary) return nullptr;

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
        col->children.push_back(std::shared_ptr<dftu_series>(fields[i]));
        col->field_names.push_back(names && names[i] ? names[i] : "");
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
    col->child = std::shared_ptr<dftu_series>(values);
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
        col->child) {
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
    if (!col->offsets) return nullptr;
    return reinterpret_cast<const int32_t*>(col->offsets->data());
}

const int64_t* dftu_series_offsets64(const dftu_series* col) {
    if (!col->offsets64) return nullptr;
    return reinterpret_cast<const int64_t*>(col->offsets64->data());
}

int32_t dftu_series_is_null(const dftu_series* col, int64_t i) {
    // A SELECTION view carries no bitmap of its own: the row is null when its
    // index is the outer-fill sentinel or the selected base row is null.
    if (col->encoding == dftracer::utils::dataframe::Encoding::Selection &&
        col->child) {
        const std::int64_t idx =
            reinterpret_cast<const std::int64_t*>(col->data->data())[i];
        if (idx < 0) return 1;
        return dftu_series_is_null(col->child.get(), idx);
    }
    if (!col->validity) return 0;
    const std::uint8_t* bm = col->validity->data();
    return ((bm[i >> 3] >> (i & 7)) & 1) ? 0 : 1;
}

namespace {
bool is_single_child_container(TypeId t) {
    return t == TypeId::List || t == TypeId::LargeList ||
           t == TypeId::FixedSizeList || t == TypeId::Map;
}
}  // namespace

int32_t dftu_series_num_children(const dftu_series* col) {
    if (is_single_child_container(col->type)) return col->child ? 1 : 0;
    return static_cast<int32_t>(col->children.size());
}

dftu_series* dftu_series_child(const dftu_series* col, int32_t i) {
    const std::shared_ptr<dftu_series>* ch = nullptr;
    if (is_single_child_container(col->type)) {
        if (i == 0 && col->child) ch = &col->child;
    } else if (i >= 0 && static_cast<std::size_t>(i) < col->children.size()) {
        ch = &col->children[static_cast<std::size_t>(i)];
    }
    if (!ch || !*ch) return nullptr;
    // Owned copy sharing the child's buffers (shared_ptr members).
    return new dftu_series(**ch);
}

const char* dftu_series_field_name(const dftu_series* col, int32_t i) {
    if (!col || col->type != TypeId::Struct) return nullptr;
    if (i < 0 || static_cast<std::size_t>(i) >= col->field_names.size())
        return nullptr;
    return col->field_names[static_cast<std::size_t>(i)].c_str();
}

int32_t dftu_series_time_unit(const dftu_series* col) {
    return col ? static_cast<int32_t>(col->time_unit)
               : static_cast<int32_t>(
                     dftracer::utils::dataframe::TimeUnit::Micro);
}

const char* dftu_series_timezone(const dftu_series* col) {
    static const char empty[] = "";
    return col ? col->timezone.c_str() : empty;
}

int32_t dftu_series_decimal_precision(const dftu_series* col) {
    return col ? col->decimal_precision : 0;
}

int32_t dftu_series_decimal_scale(const dftu_series* col) {
    return col ? col->decimal_scale : 0;
}

int32_t dftu_series_fixed_size(const dftu_series* col) {
    return col ? col->fixed_size : 0;
}

dftu_series* dftu_series_share(const dftu_series* col) {
    if (!col) return nullptr;
    // Copy the handle struct; its buffer/child shared_ptr members bump their
    // refcounts, so the new column shares the same data with no copy.
    return new dftu_series(*col);
}

dftu_series* dftu_series_slice(const dftu_series* col, int64_t offset,
                               int64_t len) {
    if (!col) return nullptr;
    if (byte_width(col->type, col->fixed_size).value_or(0) == 0)
        return nullptr;  // variable-width unsupported
    if (col->encoding == Encoding::Selection && col->child) {
        // A SELECTION slices by its index buffer: still a view over the base.
        if (offset < 0) offset = 0;
        if (offset > col->length) offset = col->length;
        if (len < 0 || len > col->length - offset) len = col->length - offset;
        auto* out = new dftu_series();
        dftracer::utils::dataframe::adopt_type_from(*out, *col);
        out->encoding = Encoding::Selection;
        out->length = len;
        out->data = Buffer::allocate(static_cast<std::size_t>(len) *
                                     sizeof(std::int64_t));
        std::memcpy(out->data->data(),
                    col->data->data() +
                        static_cast<std::size_t>(offset) * sizeof(std::int64_t),
                    static_cast<std::size_t>(len) * sizeof(std::int64_t));
        out->child = col->child;
        return out;
    }
    if (col->encoding != Encoding::Flat) {
        dftu_series* flat = dftu_series_materialize(col);
        if (!flat) return nullptr;
        dftu_series* out = dftu_series_slice(flat, offset, len);
        dftu_series_free(flat);
        return out;
    }
    const std::size_t w = byte_width(col->type, col->fixed_size).value_or(0);
    if (w == 0 || !col->data) return nullptr;  // variable-width unsupported
    if (offset < 0) offset = 0;
    if (offset > col->length) offset = col->length;
    if (len < 0 || len > col->length - offset) len = col->length - offset;

    auto* out = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*out, *col);
    out->encoding = Encoding::Flat;
    out->length = len;
    auto parent = col->data;  // shared_ptr copy keeps the buffer alive
    if (col->type == TypeId::Bool) {
        // Bool is bit-packed ((n+7)/8 bytes)
        const std::size_t nbytes = static_cast<std::size_t>((len + 7) / 8);
        auto dbuf = Buffer::allocate(nbytes);
        std::memset(dbuf->data(), 0, nbytes);
        const std::uint8_t* src = parent->data();
        std::uint8_t* dst = dbuf->data();
        for (int64_t i = 0; i < len; ++i) {
            const int64_t p = offset + i;
            if ((src[p >> 3] >> (p & 7)) & 1u)
                dst[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        }
        out->data = std::move(dbuf);
    } else {
        std::uint8_t* base =
            parent->data() + static_cast<std::size_t>(offset) * w;
        out->data =
            Buffer::wrap(base, static_cast<std::size_t>(len) * w,
                         [parent](void*) { /* view: parent owns it */ });
    }

    // Carry the validity bitmap so nulls survive into the expression engine.
    // The bitmap is indexed from bit 0, so a byte-aligned offset can share the
    // parent buffer at a shifted base; otherwise re-pack the [offset, offset+
    // len) bits down to bit 0.
    if (col->validity && len > 0) {
        const std::uint8_t* src = col->validity->data();
        std::shared_ptr<Buffer> vbuf;
        if ((offset & 7) == 0) {
            auto vparent = col->validity;
            std::uint8_t* vbase =
                vparent->data() + static_cast<std::size_t>(offset >> 3);
            vbuf = Buffer::wrap(vbase, static_cast<std::size_t>((len + 7) / 8),
                                [vparent](void*) { /* view */ });
        } else {
            const std::size_t nbytes = static_cast<std::size_t>((len + 7) / 8);
            vbuf = Buffer::allocate(nbytes);
            std::memset(vbuf->data(), 0, nbytes);
            std::uint8_t* dst = vbuf->data();
            for (int64_t i = 0; i < len; ++i) {
                const int64_t p = offset + i;
                if ((src[p >> 3] >> (p & 7)) & 1u)
                    dst[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
            }
        }
        // Recount nulls in the slice; the parent's count spans the whole
        // column.
        out->null_count = count_nulls(vbuf->data(), len);
        out->validity = std::move(vbuf);
    }
    return out;
}
