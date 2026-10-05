#include <dftracer/utils/dataframe/kernels/cast.h>
#include <dftracer/utils/dataframe/parallel.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/cast_simd.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::dataframe {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Same-width lane conversion (int <-> float of equal byte width): Highway's
// ConvertTo matches C++ static_cast (truncation toward zero for float->int).
template <class S, class D>
void ConvertSameWidth(const void* sv, void* dv, std::size_t n) {
    const S* s = static_cast<const S*>(sv);
    D* d = static_cast<D*>(dv);
    const hn::ScalableTag<S> ds;
    const hn::Rebind<D, decltype(ds)> dd;
    const std::size_t lanes = hn::Lanes(ds);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes)
        hn::StoreU(hn::ConvertTo(dd, hn::LoadU(ds, s + i)), dd, d + i);
    for (; i < n; ++i) d[i] = static_cast<D>(s[i]);
}

// float32 -> float64 (promote) and float64 -> float32 (demote).
void PromoteF32F64(const void* sv, void* dv, std::size_t n) {
    const float* s = static_cast<const float*>(sv);
    double* d = static_cast<double*>(dv);
    const hn::ScalableTag<double> dd;
    const hn::Rebind<float, decltype(dd)> df;
    const std::size_t lanes = hn::Lanes(dd);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes)
        hn::StoreU(hn::PromoteTo(dd, hn::LoadU(df, s + i)), dd, d + i);
    for (; i < n; ++i) d[i] = static_cast<double>(s[i]);
}

void DemoteF64F32(const void* sv, void* dv, std::size_t n) {
    const double* s = static_cast<const double*>(sv);
    float* d = static_cast<float*>(dv);
    const hn::ScalableTag<double> dd;
    const hn::Rebind<float, decltype(dd)> df;
    const std::size_t lanes = hn::Lanes(dd);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes)
        hn::StoreU(hn::DemoteTo(df, hn::LoadU(dd, s + i)), df, d + i);
    for (; i < n; ++i) d[i] = static_cast<float>(s[i]);
}

// int32 -> float64 (promote: widen lanes, then exact convert).
void CastI32F64(const void* sv, void* dv, std::size_t n) {
    const std::int32_t* s = static_cast<const std::int32_t*>(sv);
    double* d = static_cast<double*>(dv);
    const hn::ScalableTag<double> dd;
    const hn::Rebind<std::int32_t, decltype(dd)> di;
    const std::size_t lanes = hn::Lanes(dd);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes)
        hn::StoreU(hn::PromoteTo(dd, hn::LoadU(di, s + i)), dd, d + i);
    for (; i < n; ++i) d[i] = static_cast<double>(s[i]);
}

void CastI32F32(const void* s, void* d, std::size_t n) {
    ConvertSameWidth<std::int32_t, float>(s, d, n);
}
void CastF32I32(const void* s, void* d, std::size_t n) {
    ConvertSameWidth<float, std::int32_t>(s, d, n);
}
void CastI64F64(const void* s, void* d, std::size_t n) {
    ConvertSameWidth<std::int64_t, double>(s, d, n);
}
void CastF64I64(const void* s, void* d, std::size_t n) {
    ConvertSameWidth<double, std::int64_t>(s, d, n);
}

// Number -> Bool: one packed result bit per row, 64 rows a word (LSB first, as
// the column's bit order). A float also writes a validity bit per row, so a NaN
// is null and not silently true. `bits` and `valid` are zeroed by the caller.
template <class T, bool kFloat>
void NonzeroBits(const void* sv, std::uint8_t* bits, std::uint8_t* valid,
                 std::size_t n) {
    const T* s = static_cast<const T*>(sv);
    // At most 64 lanes, so one vector's mask fits the 64-bit word below.
    const hn::CappedTag<T, 64> d;
    const std::size_t lanes = hn::Lanes(d);
    const auto zero = hn::Zero(d);
    const std::uint64_t lane_bits =
        lanes == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << lanes) - 1;
    std::size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        std::uint64_t data_word = 0;
        std::uint64_t valid_word = 0;
        for (std::size_t c = 0; c < 64; c += lanes) {
            const auto v = hn::LoadU(d, s + i + c);
            auto nonzero = hn::Ne(v, zero);
            if constexpr (kFloat) {
                const auto ordered = hn::Eq(v, v);
                std::uint8_t vb[8] = {};
                hn::StoreMaskBits(d, ordered, vb);
                std::uint64_t vw = 0;
                std::memcpy(&vw, vb, 8);
                valid_word |= (vw & lane_bits) << c;
                nonzero = hn::And(nonzero, ordered);
            }
            std::uint8_t db[8] = {};
            hn::StoreMaskBits(d, nonzero, db);
            std::uint64_t dw = 0;
            std::memcpy(&dw, db, 8);
            data_word |= (dw & lane_bits) << c;
        }
        std::memcpy(bits + (i >> 3), &data_word, 8);
        if constexpr (kFloat) std::memcpy(valid + (i >> 3), &valid_word, 8);
    }
    for (; i < n; ++i) {
        const T x = s[i];
        bool ordered = true;
        if constexpr (kFloat) ordered = (x == x);
        if (!ordered) continue;
        if constexpr (kFloat)
            valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        if (x != T{}) bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
}

void NonzeroBitsI32(const void* s, std::uint8_t* b, std::uint8_t* v,
                    std::size_t n) {
    NonzeroBits<std::int32_t, false>(s, b, v, n);
}
void NonzeroBitsI64(const void* s, std::uint8_t* b, std::uint8_t* v,
                    std::size_t n) {
    NonzeroBits<std::int64_t, false>(s, b, v, n);
}
void NonzeroBitsF32(const void* s, std::uint8_t* b, std::uint8_t* v,
                    std::size_t n) {
    NonzeroBits<float, true>(s, b, v, n);
}
void NonzeroBitsF64(const void* s, std::uint8_t* b, std::uint8_t* v,
                    std::size_t n) {
    NonzeroBits<double, true>(s, b, v, n);
}

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::dataframe
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::dataframe {

HWY_EXPORT(PromoteF32F64);
HWY_EXPORT(DemoteF64F32);
HWY_EXPORT(CastI32F64);
HWY_EXPORT(CastI32F32);
HWY_EXPORT(CastF32I32);
HWY_EXPORT(CastI64F64);
HWY_EXPORT(CastF64I64);
HWY_EXPORT(NonzeroBitsI32);
HWY_EXPORT(NonzeroBitsI64);
HWY_EXPORT(NonzeroBitsF32);
HWY_EXPORT(NonzeroBitsF64);

std::int64_t cast_bool_simd(std::int32_t src, const void* sv,
                            std::uint8_t* bits, std::uint8_t* valid,
                            std::size_t n) {
    const TypeId s = static_cast<TypeId>(src);
    if (n == 0) return 0;
    switch (s) {
        case TypeId::Int32:
            HWY_DYNAMIC_DISPATCH(NonzeroBitsI32)(sv, bits, valid, n);
            return 0;
        case TypeId::Int64:
            HWY_DYNAMIC_DISPATCH(NonzeroBitsI64)(sv, bits, valid, n);
            return 0;
        case TypeId::Float32:
            HWY_DYNAMIC_DISPATCH(NonzeroBitsF32)(sv, bits, valid, n);
            break;
        case TypeId::Float64:
            HWY_DYNAMIC_DISPATCH(NonzeroBitsF64)(sv, bits, valid, n);
            break;
        default:
            return -1;
    }
    // A float: the rows with no validity bit are the NaN rows. Count the set
    // bits a byte at a time, and the last partial byte under its mask.
    std::int64_t ok = 0;
    const std::size_t full = n >> 3;
    for (std::size_t b = 0; b < full; ++b) ok += std::popcount(valid[b]);
    if (n & 7)
        ok += std::popcount(
            static_cast<std::uint8_t>(valid[full] & ((1u << (n & 7)) - 1u)));
    return static_cast<std::int64_t>(n) - ok;
}

// Vectorize the numeric casts Highway maps directly onto a single ConvertTo /
// Promote / Demote (float <-> same-width int, and float widen/narrow) - the
// hot expr float-promotion path (i64/i32 columns cast to Float64/Float32 on
// every mixed-type or division expression). Returns false for the exotic
// width-changing integer pairs, which stay on the scalar fallback in cast.cpp.
bool cast_simd(std::int32_t src, std::int32_t dst, const void* sv, void* dv,
               std::size_t n) {
    const TypeId s = static_cast<TypeId>(src);
    const TypeId d = static_cast<TypeId>(dst);
    if (n == 0) return true;
    if (s == TypeId::Float32 && d == TypeId::Float64) {
        HWY_DYNAMIC_DISPATCH(PromoteF32F64)(sv, dv, n);
        return true;
    }
    if (s == TypeId::Float64 && d == TypeId::Float32) {
        HWY_DYNAMIC_DISPATCH(DemoteF64F32)(sv, dv, n);
        return true;
    }
    if (s == TypeId::Int32 && d == TypeId::Float32) {
        HWY_DYNAMIC_DISPATCH(CastI32F32)(sv, dv, n);
        return true;
    }
    if (s == TypeId::Float32 && d == TypeId::Int32) {
        HWY_DYNAMIC_DISPATCH(CastF32I32)(sv, dv, n);
        return true;
    }
    if (s == TypeId::Int64 && d == TypeId::Float64) {
        // EXPERIMENT (op 2 measurement, see dataframe_parallel_bench.cpp):
        // fan out across row ranges to measure whether this bandwidth-bound
        // cast is worth parallelizing on this machine.
        constexpr std::size_t CAST_PARALLEL_GRAIN = 1 << 20;
        if (parallel_backend_installed() && n >= CAST_PARALLEL_GRAIN) {
            const auto* s64 = static_cast<const std::int64_t*>(sv);
            auto* d64 = static_cast<double*>(dv);
            parallel_for(static_cast<std::int64_t>(n),
                         static_cast<std::int64_t>(CAST_PARALLEL_GRAIN),
                         [&](std::int64_t beg, std::int64_t end) {
                             HWY_DYNAMIC_DISPATCH(CastI64F64)
                             (s64 + beg, d64 + beg,
                              static_cast<std::size_t>(end - beg));
                         });
        } else {
            HWY_DYNAMIC_DISPATCH(CastI64F64)(sv, dv, n);
        }
        return true;
    }
    if (s == TypeId::Int32 && d == TypeId::Float64) {
        HWY_DYNAMIC_DISPATCH(CastI32F64)(sv, dv, n);
        return true;
    }
    if (s == TypeId::Float64 && d == TypeId::Int64) {
        HWY_DYNAMIC_DISPATCH(CastF64I64)(sv, dv, n);
        return true;
    }
    return false;
}

}  // namespace dftracer::utils::dataframe
#endif
