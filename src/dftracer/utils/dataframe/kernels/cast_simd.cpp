#include <dftracer/utils/dataframe/kernels/cast.h>
#include <dftracer/utils/dataframe/parallel.h>

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
