// SPDX-License-Identifier: MIT
// IEEE-class predicate masks (is_nan / is_finite / is_infinite) as a Highway
// dataframe kernel over a FLAT float column, producing a bit-packed Bool
// column. Integer columns never carry NaN/Inf, so those are filled without a
// kernel.

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/kernels/nan_rows.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/bits.h>

#include <algorithm>
#include <bit>
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

// mode: 0 = is_nan, 1 = is_finite, 2 = is_infinite. Bits are built 64 at a time
// so every block store lands on a byte boundary regardless of the lane count;
// the shorter tail is packed scalar. Mirrors compare_simd's compare_bits.
template <class T>
void pred_bits(const T* p, std::int64_t n, std::int32_t mode,
               std::uint8_t* out) {
    const hwy::HWY_NAMESPACE::ScalableTag<T> d;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    const std::uint64_t lane_mask =
        lanes >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << lanes) - 1);
    std::int64_t i = 0;
    for (; i + 64 <= n; i += 64) {
        std::uint64_t bits = 0;
        for (std::size_t c = 0; c < 64; c += lanes) {
            const auto v = hwy::HWY_NAMESPACE::LoadU(
                d, p + i + static_cast<std::int64_t>(c));
            const auto m = mode == 0   ? hwy::HWY_NAMESPACE::IsNaN(v)
                           : mode == 1 ? hwy::HWY_NAMESPACE::IsFinite(v)
                                       : hwy::HWY_NAMESPACE::IsInf(v);
            std::uint64_t cb = 0;
            hwy::HWY_NAMESPACE::StoreMaskBits(
                d, m, reinterpret_cast<std::uint8_t*>(&cb));
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
    out->data = new_buffer(bytes == 0 ? 1 : bytes);
    std::memset(writable(out->data), 0, out->data.size());
    return out;
}

// mode: 0 = is_nan, 1 = is_finite, 2 = is_infinite.
dftu_series* predicate_mask(const dftu_series* v, std::int32_t mode) {
    if (v->encoding != Encoding::Flat || !is_numeric(v->type)) return nullptr;
    dftu_series* out = new_bool(v->length);
    if (v->type == TypeId::Float32) {
        HWY_DYNAMIC_DISPATCH(PredF32)
        (v->data.data(), v->length, mode, writable(out->data));
    } else if (v->type == TypeId::Float64) {
        HWY_DYNAMIC_DISPATCH(PredF64)
        (v->data.data(), v->length, mode, writable(out->data));
    } else if (mode == 1) {
        // Integer/Bool columns are all-finite (never NaN/Inf); trailing bits of
        // the last byte are ignored by the length.
        std::memset(writable(out->data), 0xFF, out->data.size());
    }
    // mode 0/2 on an integer column: all false, already zeroed.
    // A null value has a null answer, as every elementwise op.
    out->validity = v->validity;
    out->null_count = v->null_count;
    return out;
}

bool is_float(TypeId t) { return t == TypeId::Float32 || t == TypeId::Float64; }

// A Bool column of the rows that are present and NaN, for a FLAT float column.
dftu_series* nan_rows(const dftu_series* v) {
    dftu_series* out = predicate_mask(v, 0);
    out->validity = BufferView{};
    out->null_count = 0;
    if (v->validity.block()) {
        std::uint8_t* dst = writable(out->data);
        const std::uint8_t* valid = v->validity.data();
        const std::size_t bytes =
            std::min(buffer_bytes(TypeId::Bool, v->length), v->validity.size());
        for (std::size_t i = 0; i < bytes; ++i) dst[i] &= valid[i];
    }
    return out;
}

// The Bool mask of the rows that are not a present NaN of a FLAT float column,
// in one pass over the packed bits; `*nan_count` counts the rows that are.
dftu_series* keep_non_nan_flat(const dftu_series* v, std::int64_t* nan_count) {
    dftu_series* out = predicate_mask(v, 0);
    out->validity = BufferView{};
    out->null_count = 0;
    std::uint8_t* bits = writable(out->data);
    const std::uint8_t* valid =
        v->validity.block() ? v->validity.data() : nullptr;
    std::size_t bytes = std::min(buffer_bytes(TypeId::Bool, v->length),
                                 static_cast<std::size_t>(out->data.size()));
    if (valid)
        bytes = std::min(bytes, static_cast<std::size_t>(v->validity.size()));
    std::int64_t nans = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        const std::uint8_t m =
            valid ? static_cast<std::uint8_t>(bits[i] & valid[i]) : bits[i];
        nans += std::popcount(m);
        bits[i] = static_cast<std::uint8_t>(~m);
    }
    *nan_count = nans;
    return out;
}

// Whether a FLAT float column has a NaN in a row that is not null.
template <class T>
bool any_present_nan_flat(const dftu_series* v) {
    const T* in = reinterpret_cast<const T*>(v->data.data());
    const auto n = static_cast<std::size_t>(v->length);
    const std::uint8_t* valid =
        v->validity.block() ? v->validity.data() : nullptr;
    for (std::size_t i = 0; i < n; ++i)
        if (in[i] != in[i] && (!valid || ((valid[i >> 3] >> (i & 7)) & 1)))
            return true;
    return false;
}

// Whether a float column may have a NaN in a row that is not null, read in
// place. A layout other than FLAT, CHUNKED or a SELECTION over a FLAT base
// answers yes without looking.
bool may_have_present_nan(const dftu_series* v) {
    if (v->encoding == Encoding::Flat)
        return v->type == TypeId::Float32 ? any_present_nan_flat<float>(v)
                                          : any_present_nan_flat<double>(v);
    if (v->is_chunked()) {
        for (const auto& c : v->nested)
            if (may_have_present_nan(c.series.get())) return true;
        return false;
    }
    const dftu_series* base = v->child().get();
    if (v->encoding != Encoding::Selection || !base ||
        base->encoding != Encoding::Flat)
        return true;
    const auto* idx = reinterpret_cast<const std::int64_t*>(v->data.data());
    for (std::int64_t i = 0; i < v->length; ++i) {
        if (idx[i] < 0 || dftu_series_is_null(v, i)) continue;
        const auto k = static_cast<std::size_t>(idx[i]);
        const bool nan =
            base->type == TypeId::Float32
                ? reinterpret_cast<const float*>(base->data.data())[k] !=
                      reinterpret_cast<const float*>(base->data.data())[k]
                : reinterpret_cast<const double*>(base->data.data())[k] !=
                      reinterpret_cast<const double*>(base->data.data())[k];
        if (nan) return true;
    }
    return false;
}

// NaN of a FLAT float column replaced by `fv`, or by a null. A column with no
// NaN is shared, not copied.
template <class T>
dftu_series* fill_nan_flat(const dftu_series* v, T fv, bool to_null) {
    const T* in = reinterpret_cast<const T*>(v->data.data());
    const auto n = static_cast<std::size_t>(v->length);
    if (!any_present_nan_flat<T>(v)) return dftu_series_share(v);
    auto* out = new dftu_series();
    out->type = v->type;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->data = new_buffer(buffer_bytes(v->type, v->length));
    T* dst = reinterpret_cast<T*>(writable(out->data));
    if (!to_null) {
        for (std::size_t i = 0; i < n; ++i)
            dst[i] = in[i] == in[i] ? in[i] : fv;
        out->validity = v->validity;
        out->null_count = v->null_count;
        return out;
    }
    if (n) std::memcpy(dst, in, n * sizeof(T));
    const std::size_t bytes = (n + 7) / 8;
    BufferView bitmap = new_buffer(bytes == 0 ? 1 : bytes);
    std::uint8_t* bits = writable(bitmap);
    std::memset(bits, 0, bitmap.size());
    const std::uint8_t* valid =
        v->validity.block() ? v->validity.data() : nullptr;
    for (std::size_t i = 0; i < n; ++i) {
        const bool present = !valid || ((valid[i >> 3] >> (i & 7)) & 1);
        if (present && in[i] == in[i])
            bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
    out->null_count = v->length - static_cast<std::int64_t>(
                                      dstd::bits::popcount_bytes(bits, bytes));
    out->validity = std::move(bitmap);
    return out;
}

}  // namespace

}  // namespace dftracer::utils::dataframe

using namespace dftracer::utils::dataframe;

namespace {

dftu_series* float_class_mask(const dftu_series* v,
                              dftu_series* (*self)(const dftu_series*),
                              std::int32_t mode) {
    if (!v) return nullptr;
    DFTU_DICTIONARY_ELEMENTWISE(v, self);
    DFTU_FLAT_OPERAND(v, flat_v, self(flat_v));
    return predicate_mask(v, mode);
}

}  // namespace

dftu_series* dftu_series_is_nan(const dftu_series* v) {
    return float_class_mask(v, dftu_series_is_nan, 0);
}
dftu_series* dftu_series_is_finite(const dftu_series* v) {
    return float_class_mask(v, dftu_series_is_finite, 1);
}
dftu_series* dftu_series_is_infinite(const dftu_series* v) {
    return float_class_mask(v, dftu_series_is_infinite, 2);
}

namespace {

// The validity bitmap as a Bool column (inverted for null_mask). A SELECTION,
// and a DICTIONARY with null entries, is read row by row through is_null; on
// every other layout validity is per row. A column with no bitmap has no
// nulls.
dftu_series* validity_mask(const dftu_series* v,
                           dftu_series* (*self)(const dftu_series*),
                           bool valid) {
    if (!v) return nullptr;
    DFTU_PER_CHUNK(v, self);
    dftu_series* out = new_bool(v->length);
    if (v->rowwise_nulls()) {
        std::uint8_t* dst = writable(out->data);
        for (std::int64_t i = 0; i < v->length; ++i)
            if (dftu_series_is_null(v, i) != valid)
                dst[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        return out;
    }
    if (!v->validity.block()) {
        if (valid) std::memset(writable(out->data), 0xFF, out->data.size());
        return out;
    }
    const std::uint8_t* bm = v->validity.data();
    std::uint8_t* dst = writable(out->data);
    const std::size_t bytes =
        std::min(buffer_bytes(TypeId::Bool, v->length), v->validity.size());
    if (valid) {
        std::memcpy(dst, bm, bytes);
    } else {
        for (std::size_t i = 0; i < bytes; ++i)
            dst[i] = static_cast<std::uint8_t>(~bm[i]);
    }
    return out;
}

}  // namespace

dftu_series* dftu_series_valid_mask(const dftu_series* v) {
    return validity_mask(v, dftu_series_valid_mask, true);
}

dftu_series* dftu_series_null_mask(const dftu_series* v) {
    return validity_mask(v, dftu_series_null_mask, false);
}

namespace {

// A null mask or valid mask, combined with the NaN rows of a float column:
// null or NaN (`na`), or neither. A column that is not a float has no NaN, so
// the answer is the plain mask.
dftu_series* nan_aware_mask(const dftu_series* v,
                            dftu_series* (*self)(const dftu_series*),
                            dftu_series* (*mask)(const dftu_series*), bool na) {
    if (!v) return nullptr;
    if (!is_float(v->type)) return mask(v);
    DFTU_FLAT_OPERAND(v, flat_v, self(flat_v));
    dftu_series* out = mask(v);
    dftu_series* nan = nan_rows(v);
    std::uint8_t* dst = writable(out->data);
    const std::uint8_t* src = nan->data.data();
    const std::size_t bytes = out->data.size();
    if (na) {
        for (std::size_t i = 0; i < bytes; ++i)
            dst[i] = static_cast<std::uint8_t>(dst[i] | src[i]);
    } else {
        for (std::size_t i = 0; i < bytes; ++i)
            dst[i] = static_cast<std::uint8_t>(dst[i] & ~src[i]);
    }
    dftu_series_free(nan);
    return out;
}

}  // namespace

dftu_series* dftu_series_na_mask(const dftu_series* v) {
    return nan_aware_mask(v, dftu_series_na_mask, dftu_series_null_mask, true);
}

dftu_series* dftu_series_notna_mask(const dftu_series* v) {
    return nan_aware_mask(v, dftu_series_notna_mask, dftu_series_valid_mask,
                          false);
}

dftu_series* dftu_series_fill_nan(const dftu_series* v, dftu_scalar fill) {
    if (!v) return nullptr;
    if (!is_float(v->type)) return dftu_series_share(v);
    if (fill.kind == DFTU_SCALAR_TAG_STR || fill.kind == DFTU_SCALAR_TAG_ERR)
        return nullptr;
    if (v->encoding != Encoding::Flat && !may_have_present_nan(v))
        return dftu_series_share(v);
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_fill_nan(flat_v, fill));
    const bool to_null = fill.kind == DFTU_SCALAR_TAG_NULL;
    if (v->type == TypeId::Float32)
        return fill_nan_flat<float>(v, scalar_value<float>(fill), to_null);
    return fill_nan_flat<double>(v, scalar_value<double>(fill), to_null);
}

// A null is not a NaN, so the null rows stay.
dftu_series* dftu_series_drop_nans(const dftu_series* v) {
    if (!v) return nullptr;
    if (!is_float(v->type)) return dftu_series_share(v);
    if (v->encoding != Encoding::Flat && !may_have_present_nan(v))
        return dftu_series_share(v);
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_drop_nans(flat_v));
    std::int64_t nans = 0;
    dftu_series* keep = keep_non_nan_flat(v, &nans);
    dftu_series* out =
        nans == 0 ? dftu_series_share(v) : dftu_series_filter(v, keep);
    dftu_series_free(keep);
    return out;
}

namespace dftracer::utils::dataframe {

Series keep_non_nan(const Series& c, std::int64_t* nan_count) {
    *nan_count = 0;
    if (!c.valid() || !is_float(c.type())) return Series{};
    const dftu_series* h = c.handle();
    if (h->encoding == Encoding::Flat)
        return Series{keep_non_nan_flat(h, nan_count)};
    if (!may_have_present_nan(h)) return Series{};
    dftu_series* flat = dftu_series_materialize(h);
    if (!flat) {
        *nan_count = -1;
        return Series{};
    }
    Series out{keep_non_nan_flat(flat, nan_count)};
    dftu_series_free(flat);
    return out;
}

}  // namespace dftracer::utils::dataframe

#endif  // HWY_ONCE
