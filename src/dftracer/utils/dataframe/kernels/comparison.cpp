#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/compare_simd.h>
#include <dftracer/utils/dataframe/internal/numeric_dispatch.h>
#include <dftracer/utils/dataframe/internal/scalar.h>
#include <dftracer/utils/dataframe/kernels/comparison.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/series.h>

#include <cstring>

namespace dftracer::utils::dataframe {
namespace {

// Compare in the column's own type domain (no double round-trip), so 64-bit
// integers compare exactly. The scalar is converted to the column type T.
// `op` is validated at the ABI boundary (dftu_series_compare) before dispatch.
template <class T>
void compare_impl(const void* data, std::int64_t length, std::int32_t op,
                  dftu_scalar rhs, std::uint8_t* out) {
    const T* p = static_cast<const T*>(data);
    const T r = scalar_as<T>(rhs);
    for (std::int64_t i = 0; i < length; ++i) {
        bool res = false;
        switch (static_cast<CmpOp>(op)) {
            case CmpOp::Gt:
                res = p[i] > r;
                break;
            case CmpOp::Ge:
                res = p[i] >= r;
                break;
            case CmpOp::Lt:
                res = p[i] < r;
                break;
            case CmpOp::Le:
                res = p[i] <= r;
                break;
            case CmpOp::Eq:
                res = p[i] == r;
                break;
            case CmpOp::Ne:
                res = p[i] != r;
                break;
        }
        if (res) out[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
}

}  // namespace
}  // namespace dftracer::utils::dataframe

dftu_series* dftu_series_compare(const dftu_series* v, dftu_cmp_op op,
                                 dftu_scalar rhs) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_compare(flat_v, op, rhs));

    using dftracer::utils::dataframe::Buffer;
    using dftracer::utils::dataframe::buffer_bytes;
    using dftracer::utils::dataframe::CmpOp;
    using dftracer::utils::dataframe::compare_impl;
    using dftracer::utils::dataframe::Encoding;
    using dftracer::utils::dataframe::TypeId;
    // A STR rhs is a string comparison, which is a different kernel: equality
    // only, and dictionary-aware (dftu_series_str_eq tests each dictionary
    // entry once and then compares codes, rather than resolving per row).
    if (rhs.kind == DFTU_SCALAR_TAG_STR) {
        if (op != static_cast<int32_t>(CmpOp::Eq) &&
            op != static_cast<int32_t>(CmpOp::Ne))
            return nullptr;
        dftu_series* eq =
            dftu_series_str_eq(v, rhs.value.s, static_cast<int32_t>(rhs.len));
        if (eq == nullptr || op == static_cast<int32_t>(CmpOp::Eq)) return eq;
        dftu_series* ne = dftu_series_logical_not(eq);
        dftu_series_free(eq);
        return ne;
    }

    if (v->encoding != Encoding::Flat) return nullptr;
    const TypeId phys = physical_type(v->type);
    if (!is_numeric_dispatchable(phys)) return nullptr;
    if (op < static_cast<int32_t>(CmpOp::Gt) ||
        op > static_cast<int32_t>(CmpOp::Ne))
        return nullptr;

    auto* out = new dftu_series();
    out->type = TypeId::Bool;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;  // shared: nulls propagate

    std::size_t bytes = buffer_bytes(TypeId::Bool, v->length);
    out->data = Buffer::allocate(bytes);
    std::memset(out->data->data(), 0, bytes);
    std::uint8_t* bits = out->data->data();
    const void* data = v->data->data();
    std::int64_t n = v->length;

    // Chunks of whole 64-bit words: each writes its own bytes of the bitmap,
    // so the SIMD kernel runs on every core over a disjoint row range.
    const std::size_t width = byte_width(phys).value_or(0);
    dftracer::utils::dataframe::parallel_for(
        n, std::int64_t{1} << 16, [&](std::int64_t b, std::int64_t e) {
            const auto* pd = static_cast<const std::uint8_t*>(data) +
                             static_cast<std::size_t>(b) * width;
            std::uint8_t* pb = bits + (b >> 3);
            if (!dftracer::utils::dataframe::compare(v->type, pd, e - b, op,
                                                     rhs, pb)) {
                DF_NUMERIC_DISPATCH(phys, compare_impl, pd, e - b, op, rhs, pb)
            }
        });
    return out;
}

namespace {

using dftracer::utils::dataframe::CmpOp;

template <class T>
bool cmp_values(std::int32_t op, T a, T b) {
    switch (static_cast<CmpOp>(op)) {
        case CmpOp::Gt:
            return a > b;
        case CmpOp::Ge:
            return a >= b;
        case CmpOp::Lt:
            return a < b;
        case CmpOp::Le:
            return a <= b;
        case CmpOp::Eq:
            return a == b;
        case CmpOp::Ne:
            return a != b;
    }
    return false;
}

// Row range [b, e) of two columns of one physical type T.
template <class T>
void compare_series_impl(const void* pa, const void* pb, std::int64_t b,
                         std::int64_t e, std::int32_t op, std::uint8_t* out) {
    const T* x = static_cast<const T*>(pa);
    const T* y = static_cast<const T*>(pb);
    for (std::int64_t i = b; i < e; ++i)
        if (cmp_values(op, x[i], y[i]))
            out[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
}

}  // namespace

dftu_series* dftu_series_compare_series(const dftu_series* a,
                                        const dftu_series* b, dftu_cmp_op op) {
    if (!a || !b || a->length != b->length) return nullptr;
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_compare_series(flat_a, b, op));
    DFTU_FLAT_OPERAND(b, flat_b, dftu_series_compare_series(a, flat_b, op));
    using dftracer::utils::dataframe::Buffer;
    using dftracer::utils::dataframe::buffer_bytes;
    using dftracer::utils::dataframe::Encoding;
    using dftracer::utils::dataframe::narrow_varwidth_type;
    using dftracer::utils::dataframe::read_bytes;
    using dftracer::utils::dataframe::read_f64;
    using dftracer::utils::dataframe::Series;
    using dftracer::utils::dataframe::TypeId;
    if (op < static_cast<int32_t>(CmpOp::Gt) ||
        op > static_cast<int32_t>(CmpOp::Ne))
        return nullptr;
    const std::int64_t n = a->length;
    const TypeId ta = physical_type(a->type), tb = physical_type(b->type);
    const bool bytes_a = narrow_varwidth_type(a->type) == TypeId::String ||
                         narrow_varwidth_type(a->type) == TypeId::Binary;
    const bool bytes_b = narrow_varwidth_type(b->type) == TypeId::String ||
                         narrow_varwidth_type(b->type) == TypeId::Binary;
    if (bytes_a != bytes_b) return nullptr;
    if (!bytes_a &&
        (!is_numeric_dispatchable(ta) || !is_numeric_dispatchable(tb)))
        return nullptr;

    auto* out = new dftu_series();
    out->type = TypeId::Bool;
    out->encoding = Encoding::Flat;
    out->length = n;
    const std::size_t bytes = buffer_bytes(TypeId::Bool, n);
    out->data = Buffer::allocate(bytes);
    std::memset(out->data->data(), 0, bytes);
    std::uint8_t* bits = out->data->data();

    // Null where either side is null: the AND of the two bitmaps.
    if (a->validity || b->validity) {
        out->validity = Buffer::allocate(bytes);
        std::uint8_t* v = out->validity->data();
        const std::uint8_t* va = a->validity ? a->validity->data() : nullptr;
        const std::uint8_t* vb = b->validity ? b->validity->data() : nullptr;
        std::int64_t nulls = 0;
        for (std::size_t i = 0; i < bytes; ++i)
            v[i] = static_cast<std::uint8_t>((va ? va[i] : 0xFF) &
                                             (vb ? vb[i] : 0xFF));
        for (std::int64_t i = 0; i < n; ++i)
            if (!((v[i >> 3] >> (i & 7)) & 1)) ++nulls;
        out->null_count = nulls;
    }

    constexpr std::int64_t GRAIN = std::int64_t{1} << 16;
    dftracer::utils::dataframe::parallel_for(
        n, GRAIN, [&](std::int64_t lo, std::int64_t hi) {
            if (bytes_a) {
                Series sa{const_cast<dftu_series*>(a)};
                Series sb{const_cast<dftu_series*>(b)};
                for (std::int64_t i = lo; i < hi; ++i)
                    if (cmp_values(op, read_bytes(sa, i), read_bytes(sb, i)))
                        bits[i >> 3] |=
                            static_cast<std::uint8_t>(1u << (i & 7));
                sa.release();
                sb.release();
            } else if (ta == tb) {
                const void* pa = a->data->data();
                const void* pb = b->data->data();
                DF_NUMERIC_DISPATCH(ta, compare_series_impl, pa, pb, lo, hi, op,
                                    bits)
            } else {
                Series sa{const_cast<dftu_series*>(a)};
                Series sb{const_cast<dftu_series*>(b)};
                for (std::int64_t i = lo; i < hi; ++i)
                    if (cmp_values(op, read_f64(sa, i), read_f64(sb, i)))
                        bits[i >> 3] |=
                            static_cast<std::uint8_t>(1u << (i & 7));
                sa.release();
                sb.release();
            }
        });
    return out;
}
