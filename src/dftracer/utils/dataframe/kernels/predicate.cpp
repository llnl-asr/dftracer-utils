// IEEE-class predicate masks (is_nan / is_finite / is_infinite) as a Highway
// dataframe kernel over a FLAT float column, producing a bit-packed Bool
// column. Integer columns never carry NaN/Inf, so those are filled without a
// kernel.

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/predicate.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::dataframe {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// mode: 0 = is_nan, 1 = is_finite, 2 = is_infinite. Bits are built 64 at a time
// so every block store lands on a byte boundary regardless of the lane count;
// the shorter tail is packed scalar. Mirrors compare_simd's compare_bits.
template <class T>
void pred_bits(const T* p, std::int64_t n, std::int32_t mode,
               std::uint8_t* out) {
    const hn::ScalableTag<T> d;
    const std::size_t lanes = hn::Lanes(d);
    const std::uint64_t lane_mask =
        lanes >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << lanes) - 1);
    std::int64_t i = 0;
    for (; i + 64 <= n; i += 64) {
        std::uint64_t bits = 0;
        for (std::size_t c = 0; c < 64; c += lanes) {
            const auto v = hn::LoadU(d, p + i + static_cast<std::int64_t>(c));
            const auto m = mode == 0   ? hn::IsNaN(v)
                           : mode == 1 ? hn::IsFinite(v)
                                       : hn::IsInf(v);
            std::uint64_t cb = 0;
            hn::StoreMaskBits(d, m, reinterpret_cast<std::uint8_t*>(&cb));
            bits |= (cb & lane_mask) << c;
        }
        std::memcpy(out + (i >> 3), &bits, 8);
    }
    for (; i < n; ++i) {
        const bool r = mode == 0   ? std::isnan(p[i])
                       : mode == 1 ? std::isfinite(p[i])
                                   : std::isinf(p[i]);
        if (r) out[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
}

void PredF32(const void* p, std::int64_t n, std::int32_t mode,
             std::uint8_t* out) {
    pred_bits<float>(static_cast<const float*>(p), n, mode, out);
}
void PredF64(const void* p, std::int64_t n, std::int32_t mode,
             std::uint8_t* out) {
    pred_bits<double>(static_cast<const double*>(p), n, mode, out);
}

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::dataframe
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::dataframe {

HWY_EXPORT(PredF32);
HWY_EXPORT(PredF64);

namespace {

bool is_numeric(TypeId t) {
    return t != TypeId::String && t != TypeId::Binary && t != TypeId::List &&
           t != TypeId::Struct;
}

// A zeroed bit-packed Bool column of `n` rows.
dftu_series* new_bool(std::int64_t n) {
    auto* out = new dftu_series();
    out->type = TypeId::Bool;
    out->encoding = Encoding::Flat;
    out->length = n;
    out->null_count = 0;
    std::size_t bytes = buffer_bytes(TypeId::Bool, n);
    out->data = Buffer::allocate(bytes == 0 ? 1 : bytes);
    std::memset(out->data->data(), 0, out->data->size());
    return out;
}

// mode: 0 = is_nan, 1 = is_finite, 2 = is_infinite.
dftu_series* predicate_mask(const dftu_series* v, std::int32_t mode) {
    if (v->encoding != Encoding::Flat || !is_numeric(v->type)) return nullptr;
    dftu_series* out = new_bool(v->length);
    if (v->type == TypeId::Float32) {
        HWY_DYNAMIC_DISPATCH(PredF32)
        (v->data->data(), v->length, mode, out->data->data());
    } else if (v->type == TypeId::Float64) {
        HWY_DYNAMIC_DISPATCH(PredF64)
        (v->data->data(), v->length, mode, out->data->data());
    } else if (mode == 1) {
        // Integer/Bool columns are all-finite (never NaN/Inf); trailing bits of
        // the last byte are ignored by the length.
        std::memset(out->data->data(), 0xFF, out->data->size());
    }
    // mode 0/2 on an integer column: all false, already zeroed.
    // A null value has a null answer, as every elementwise op.
    out->validity = v->validity;
    out->null_count = v->null_count;
    return out;
}

}  // namespace

extern "C" {

dftu_series* dftu_series_is_nan(const dftu_series* v) {
    if (!v) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_is_nan(flat_v));
    return predicate_mask(v, 0);
}
dftu_series* dftu_series_is_finite(const dftu_series* v) {
    if (!v) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_is_finite(flat_v));
    return predicate_mask(v, 1);
}
dftu_series* dftu_series_is_infinite(const dftu_series* v) {
    if (!v) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_is_infinite(flat_v));
    return predicate_mask(v, 2);
}

// The validity bitmap as a Bool column (inverted for null_mask). A SELECTION
// view has no bitmap of its own, so it is read row by row through is_null;
// on every other layout validity is per row. A column with no bitmap has no
// nulls.
dftu_series* dftu_series_valid_mask(const dftu_series* v) {
    if (!v) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_valid_mask(flat_v));
    dftu_series* out = new_bool(v->length);
    if (v->encoding == Encoding::Selection) {
        std::uint8_t* dst = out->data->data();
        for (std::int64_t i = 0; i < v->length; ++i)
            if (!dftu_series_is_null(v, i))
                dst[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        return out;
    }
    if (!v->validity) {
        std::memset(out->data->data(), 0xFF, out->data->size());
        return out;
    }
    std::memcpy(
        out->data->data(), v->validity->data(),
        std::min(buffer_bytes(TypeId::Bool, v->length), v->validity->size()));
    return out;
}

dftu_series* dftu_series_null_mask(const dftu_series* v) {
    if (!v) return nullptr;
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_null_mask(flat_v));
    dftu_series* out = new_bool(v->length);
    if (v->encoding == Encoding::Selection) {
        std::uint8_t* dst = out->data->data();
        for (std::int64_t i = 0; i < v->length; ++i)
            if (dftu_series_is_null(v, i))
                dst[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        return out;
    }
    if (!v->validity) return out;
    const std::uint8_t* bm = v->validity->data();
    std::uint8_t* dst = out->data->data();
    const std::size_t bytes =
        std::min(buffer_bytes(TypeId::Bool, v->length), v->validity->size());
    for (std::size_t i = 0; i < bytes; ++i)
        dst[i] = static_cast<std::uint8_t>(~bm[i]);
    return out;
}

}  // extern "C"

}  // namespace dftracer::utils::dataframe
#endif  // HWY_ONCE
