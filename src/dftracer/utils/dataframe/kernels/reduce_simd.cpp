#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/reduce_simd.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/reduce_simd.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::dataframe {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Two accumulators break the serial dependency the scalar fadd chain has; the
// compiler will not do this for floating point without -ffast-math.
double sum_f64(const double* p, std::int64_t n) {
    const hn::ScalableTag<double> d;
    const std::int64_t lanes = static_cast<std::int64_t>(hn::Lanes(d));
    auto acc0 = hn::Zero(d);
    auto acc1 = hn::Zero(d);
    std::int64_t i = 0;
    for (; i + 2 * lanes <= n; i += 2 * lanes) {
        acc0 = hn::Add(acc0, hn::LoadU(d, p + i));
        acc1 = hn::Add(acc1, hn::LoadU(d, p + i + lanes));
    }
    for (; i + lanes <= n; i += lanes)
        acc0 = hn::Add(acc0, hn::LoadU(d, p + i));
    double s = hn::ReduceSum(d, hn::Add(acc0, acc1));
    for (; i < n; ++i) s += p[i];
    return s;
}

// Caller guarantees n >= 1. Seeding the accumulator with p[0] is safe because
// min/max are idempotent, so folding p[0] again in the tail cannot change it.
// IsMax is a template parameter so the reduction op is fixed at compile time
// and no branch lands in the hot loop.
template <class T, bool IsMax>
T minmax(const T* p, std::int64_t n) {
    const hn::ScalableTag<T> d;
    const std::int64_t lanes = static_cast<std::int64_t>(hn::Lanes(d));
    auto acc = hn::Set(d, p[0]);
    std::int64_t i = 0;
    for (; i + lanes <= n; i += lanes) {
        const auto v = hn::LoadU(d, p + i);
        acc = IsMax ? hn::Max(acc, v) : hn::Min(acc, v);
    }
    T best = IsMax ? hn::ReduceMax(d, acc) : hn::ReduceMin(d, acc);
    for (; i < n; ++i) {
        if (IsMax) {
            if (p[i] > best) best = p[i];
        } else {
            if (p[i] < best) best = p[i];
        }
    }
    return best;
}

void SumF64(const void* p, std::int64_t n, dftu_scalar* out) {
    out->kind = DFTU_SCALAR_TAG_F64;
    out->value.d = sum_f64(static_cast<const double*>(p), n);
}

// First index whose value equals `target`, scanned SIMD (FindFirstTrue gives
// the earliest set lane in a block), so arg_min/arg_max resolve ties to the
// earliest index. Two-pass with `minmax`: find the extreme, then its first
// position.
template <class T>
std::int64_t find_first_eq(const T* p, std::int64_t n, T target) {
    const hn::ScalableTag<T> d;
    const std::int64_t lanes = static_cast<std::int64_t>(hn::Lanes(d));
    const auto vt = hn::Set(d, target);
    std::int64_t i = 0;
    for (; i + lanes <= n; i += lanes) {
        const intptr_t lane =
            hn::FindFirstTrue(d, hn::Eq(hn::LoadU(d, p + i), vt));
        if (lane >= 0) return i + static_cast<std::int64_t>(lane);
    }
    for (; i < n; ++i)
        if (p[i] == target) return i;
    return -1;
}

template <class T>
std::int64_t arg_extreme(const T* p, std::int64_t n, bool is_min) {
    if (n <= 0) return -1;
    const T target = is_min ? minmax<T, false>(p, n) : minmax<T, true>(p, n);
    return find_first_eq<T>(p, n, target);
}

void ArgExtI64(const void* p, std::int64_t n, bool is_min, std::int64_t* out) {
    *out = arg_extreme<std::int64_t>(static_cast<const std::int64_t*>(p), n,
                                     is_min);
}
void ArgExtU64(const void* p, std::int64_t n, bool is_min, std::int64_t* out) {
    *out = arg_extreme<std::uint64_t>(static_cast<const std::uint64_t*>(p), n,
                                      is_min);
}
void ArgExtF64(const void* p, std::int64_t n, bool is_min, std::int64_t* out) {
    *out = arg_extreme<double>(static_cast<const double*>(p), n, is_min);
}
void ArgExtI32(const void* p, std::int64_t n, bool is_min, std::int64_t* out) {
    *out = arg_extreme<std::int32_t>(static_cast<const std::int32_t*>(p), n,
                                     is_min);
}
void ArgExtU32(const void* p, std::int64_t n, bool is_min, std::int64_t* out) {
    *out = arg_extreme<std::uint32_t>(static_cast<const std::uint32_t*>(p), n,
                                      is_min);
}
void ArgExtF32(const void* p, std::int64_t n, bool is_min, std::int64_t* out) {
    *out = arg_extreme<float>(static_cast<const float*>(p), n, is_min);
}

template <class T>
void store_minmax(const void* p, std::int64_t n, bool is_max,
                  dftu_scalar* out) {
    const T* tp = static_cast<const T*>(p);
    T r = is_max ? minmax<T, true>(tp, n) : minmax<T, false>(tp, n);
    if constexpr (std::is_floating_point_v<T>) {
        out->kind = DFTU_SCALAR_TAG_F64;
        out->value.d = static_cast<double>(r);
    } else if constexpr (std::is_unsigned_v<T>) {
        out->kind = DFTU_SCALAR_TAG_U64;
        out->value.u = static_cast<std::uint64_t>(r);
    } else {
        out->kind = DFTU_SCALAR_TAG_I64;
        out->value.i = static_cast<std::int64_t>(r);
    }
}

void MinMaxI64(const void* p, std::int64_t n, bool m, dftu_scalar* o) {
    store_minmax<std::int64_t>(p, n, m, o);
}
void MinMaxU64(const void* p, std::int64_t n, bool m, dftu_scalar* o) {
    store_minmax<std::uint64_t>(p, n, m, o);
}
void MinMaxF64(const void* p, std::int64_t n, bool m, dftu_scalar* o) {
    store_minmax<double>(p, n, m, o);
}
void MinMaxI32(const void* p, std::int64_t n, bool m, dftu_scalar* o) {
    store_minmax<std::int32_t>(p, n, m, o);
}
void MinMaxU32(const void* p, std::int64_t n, bool m, dftu_scalar* o) {
    store_minmax<std::uint32_t>(p, n, m, o);
}
void MinMaxF32(const void* p, std::int64_t n, bool m, dftu_scalar* o) {
    store_minmax<float>(p, n, m, o);
}
void MinMaxI16(const void* p, std::int64_t n, bool m, dftu_scalar* o) {
    store_minmax<std::int16_t>(p, n, m, o);
}
void MinMaxU16(const void* p, std::int64_t n, bool m, dftu_scalar* o) {
    store_minmax<std::uint16_t>(p, n, m, o);
}
void MinMaxI8(const void* p, std::int64_t n, bool m, dftu_scalar* o) {
    store_minmax<std::int8_t>(p, n, m, o);
}
void MinMaxU8(const void* p, std::int64_t n, bool m, dftu_scalar* o) {
    store_minmax<std::uint8_t>(p, n, m, o);
}

// OR-reduce `(complement ? ~data : data) & (valid ? valid : 0xFF)` over `nfull`
// packed-bitmap bytes; returns nonzero if any resulting bit is set. Only full
// bytes are passed (every bit in range); the caller handles the partial byte.
std::uint8_t OrMaskBytes(const std::uint8_t* data, const std::uint8_t* valid,
                         std::size_t nfull, bool complement) {
    const hn::ScalableTag<std::uint8_t> d;
    const std::size_t lanes = hn::Lanes(d);
    auto acc = hn::Zero(d);
    std::size_t i = 0;
    for (; i + lanes <= nfull; i += lanes) {
        auto x = hn::LoadU(d, data + i);
        if (complement) x = hn::Not(x);
        if (valid) x = hn::And(x, hn::LoadU(d, valid + i));
        acc = hn::Or(acc, x);
    }
    std::uint8_t sacc = 0;
    for (; i < nfull; ++i) {
        std::uint8_t x =
            complement ? static_cast<std::uint8_t>(~data[i]) : data[i];
        if (valid) x = static_cast<std::uint8_t>(x & valid[i]);
        sacc = static_cast<std::uint8_t>(sacc | x);
    }
    const bool vec_set = !hn::AllTrue(d, hn::Eq(acc, hn::Zero(d)));
    return (vec_set || sacc != 0) ? 1 : 0;
}

// Lanewise running product then a horizontal fold. For i64/u64 the low-bits
// (wrapping) multiply is associative, so the result equals the sequential
// product exactly; for f64 the summation-style reassociation may differ in the
// last ULP from a strict left fold.
template <class T>
T prod_novalid(const T* p, std::int64_t n) {
    const hn::ScalableTag<T> d;
    const std::int64_t lanes = static_cast<std::int64_t>(hn::Lanes(d));
    auto acc = hn::Set(d, T{1});
    std::int64_t i = 0;
    for (; i + lanes <= n; i += lanes) acc = hn::Mul(acc, hn::LoadU(d, p + i));
    HWY_ALIGN T buf[64];
    hn::StoreU(acc, d, buf);
    T r = T{1};
    for (std::int64_t k = 0; k < lanes; ++k) r = static_cast<T>(r * buf[k]);
    for (; i < n; ++i) r = static_cast<T>(r * p[i]);
    return r;
}

void ProdI64(const void* p, std::int64_t n, dftu_scalar* o) {
    o->kind = DFTU_SCALAR_TAG_I64;
    o->value.i =
        prod_novalid<std::int64_t>(static_cast<const std::int64_t*>(p), n);
}
void ProdU64(const void* p, std::int64_t n, dftu_scalar* o) {
    o->kind = DFTU_SCALAR_TAG_U64;
    o->value.u =
        prod_novalid<std::uint64_t>(static_cast<const std::uint64_t*>(p), n);
}
void ProdF64(const void* p, std::int64_t n, dftu_scalar* o) {
    o->kind = DFTU_SCALAR_TAG_F64;
    o->value.d = prod_novalid<double>(static_cast<const double*>(p), n);
}

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::dataframe
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::dataframe {

HWY_EXPORT(SumF64);
HWY_EXPORT(MinMaxI64);
HWY_EXPORT(MinMaxU64);
HWY_EXPORT(MinMaxF64);
HWY_EXPORT(MinMaxI32);
HWY_EXPORT(MinMaxU32);
HWY_EXPORT(MinMaxF32);
HWY_EXPORT(MinMaxI16);
HWY_EXPORT(MinMaxU16);
HWY_EXPORT(MinMaxI8);
HWY_EXPORT(MinMaxU8);
HWY_EXPORT(ArgExtI64);
HWY_EXPORT(ArgExtU64);
HWY_EXPORT(ArgExtF64);
HWY_EXPORT(ArgExtI32);
HWY_EXPORT(ArgExtU32);
HWY_EXPORT(ArgExtF32);
HWY_EXPORT(OrMaskBytes);
HWY_EXPORT(ProdI64);
HWY_EXPORT(ProdU64);
HWY_EXPORT(ProdF64);

bool reduce(const dftu_series& v, std::int32_t op, dftu_scalar& out) {
    if (v.validity.block())
        return false;  // null-skip breaks vectorization; use scalar
    if (v.length == 0) return false;
    const void* p = v.data.data();
    std::int64_t n = v.length;

    if (op == DFTU_REDUCE_SUM) {
        // Only Float64 needs help; integer sums already auto-vectorize and f32
        // sum accumulates in double (scalar) to hold precision.
        if (v.type == TypeId::Float64) {
            HWY_DYNAMIC_DISPATCH(SumF64)(p, n, &out);
            return true;
        }
        return false;
    }
    if (op != DFTU_REDUCE_MIN && op != DFTU_REDUCE_MAX) return false;
    bool is_max = (op == DFTU_REDUCE_MAX);
    switch (v.type) {
        case TypeId::Int64:
            HWY_DYNAMIC_DISPATCH(MinMaxI64)(p, n, is_max, &out);
            return true;
        case TypeId::Uint64:
            HWY_DYNAMIC_DISPATCH(MinMaxU64)(p, n, is_max, &out);
            return true;
        case TypeId::Float64:
            HWY_DYNAMIC_DISPATCH(MinMaxF64)(p, n, is_max, &out);
            return true;
        case TypeId::Int32:
            HWY_DYNAMIC_DISPATCH(MinMaxI32)(p, n, is_max, &out);
            return true;
        case TypeId::Uint32:
            HWY_DYNAMIC_DISPATCH(MinMaxU32)(p, n, is_max, &out);
            return true;
        case TypeId::Float32:
            HWY_DYNAMIC_DISPATCH(MinMaxF32)(p, n, is_max, &out);
            return true;
        case TypeId::Int16:
            HWY_DYNAMIC_DISPATCH(MinMaxI16)(p, n, is_max, &out);
            return true;
        case TypeId::Uint16:
            HWY_DYNAMIC_DISPATCH(MinMaxU16)(p, n, is_max, &out);
            return true;
        case TypeId::Int8:
            HWY_DYNAMIC_DISPATCH(MinMaxI8)(p, n, is_max, &out);
            return true;
        case TypeId::Uint8:
            HWY_DYNAMIC_DISPATCH(MinMaxU8)(p, n, is_max, &out);
            return true;
        default:
            return false;
    }
}

bool arg_extreme_simd(const dftu_series& v, bool is_min,
                      std::int64_t& out_idx) {
    if (v.encoding != Encoding::Flat || static_cast<bool>(v.validity.block()) ||
        v.length == 0)
        return false;
    const void* p = v.data.data();
    const std::int64_t n = v.length;
    switch (v.type) {
        case TypeId::Int64:
            HWY_DYNAMIC_DISPATCH(ArgExtI64)(p, n, is_min, &out_idx);
            return true;
        case TypeId::Uint64:
            HWY_DYNAMIC_DISPATCH(ArgExtU64)(p, n, is_min, &out_idx);
            return true;
        case TypeId::Float64:
            HWY_DYNAMIC_DISPATCH(ArgExtF64)(p, n, is_min, &out_idx);
            return true;
        case TypeId::Int32:
            HWY_DYNAMIC_DISPATCH(ArgExtI32)(p, n, is_min, &out_idx);
            return true;
        case TypeId::Uint32:
            HWY_DYNAMIC_DISPATCH(ArgExtU32)(p, n, is_min, &out_idx);
            return true;
        case TypeId::Float32:
            HWY_DYNAMIC_DISPATCH(ArgExtF32)(p, n, is_min, &out_idx);
            return true;
        default:
            return false;
    }
}

namespace {

// Split the bool bitmap into `nfull` full bytes (SIMD path) plus a partial
// trailing byte holding `rem` in-range bits.
struct BitmapSplit {
    const std::uint8_t* data;
    const std::uint8_t* valid;  // nullptr when the column has no validity
    std::size_t nfull;
    int rem;
    std::uint8_t partial_mask;  // low `rem` bits, 0 when rem == 0
};

BitmapSplit split_bitmap(const dftu_series& v) {
    BitmapSplit s;
    s.data = v.data.data();
    s.valid = v.validity.block() ? v.validity.data() : nullptr;
    const std::int64_t length = v.length;
    s.nfull = static_cast<std::size_t>(length / 8);
    s.rem = static_cast<int>(length & 7);
    s.partial_mask =
        s.rem ? static_cast<std::uint8_t>((1u << s.rem) - 1) : std::uint8_t{0};
    return s;
}

}  // namespace

bool any_bits(const dftu_series& v) {
    const BitmapSplit s = split_bitmap(v);
    if (HWY_DYNAMIC_DISPATCH(OrMaskBytes)(s.data, s.valid, s.nfull, false))
        return true;
    if (s.rem) {
        const std::uint8_t vb = s.valid ? s.valid[s.nfull] : std::uint8_t{0xFF};
        if ((s.data[s.nfull] & vb & s.partial_mask) != 0) return true;
    }
    return false;
}

bool all_bits(const dftu_series& v) {
    const BitmapSplit s = split_bitmap(v);
    if (HWY_DYNAMIC_DISPATCH(OrMaskBytes)(s.data, s.valid, s.nfull, true))
        return false;
    if (s.rem) {
        const std::uint8_t vb = s.valid ? s.valid[s.nfull] : std::uint8_t{0xFF};
        const std::uint8_t nd = static_cast<std::uint8_t>(~s.data[s.nfull]);
        if ((nd & vb & s.partial_mask) != 0) return false;
    }
    return true;
}

bool product_simd(const dftu_series& v, dftu_scalar& out) {
    if (v.validity.block()) return false;  // null-skip stays on the scalar path
    const void* p = v.data.data();
    const std::int64_t n = v.length;
    switch (v.type) {
        case TypeId::Int64:
            HWY_DYNAMIC_DISPATCH(ProdI64)(p, n, &out);
            return true;
        case TypeId::Uint64:
            HWY_DYNAMIC_DISPATCH(ProdU64)(p, n, &out);
            return true;
        case TypeId::Float64:
            HWY_DYNAMIC_DISPATCH(ProdF64)(p, n, &out);
            return true;
        default:
            return false;  // narrow types accumulate in a wider domain: scalar
    }
}

}  // namespace dftracer::utils::dataframe
#endif  // HWY_ONCE
