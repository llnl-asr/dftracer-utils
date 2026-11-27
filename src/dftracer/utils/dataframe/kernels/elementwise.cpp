// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/numeric_dispatch.h>
#include <dftracer/utils/dataframe/internal/varwidth_offsets.h>
#include <dftracer/utils/dataframe/kernels/elementwise.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/elementwise.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>
// math-inl.h is an -inl.h: it participates in foreach_target and must follow
// highway.h. Provides the vectorized transcendentals hwy::HWY_NAMESPACE::Exp /
// hwy::HWY_NAMESPACE::Log.
#include <hwy/contrib/math/math-inl.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::dataframe {
namespace HWY_NAMESPACE {

template <class T>
void AbsImpl(const void* av, void* ov, std::size_t n) {
    const T* a = static_cast<const T*>(av);
    T* out = static_cast<T*>(ov);
    if constexpr (std::is_unsigned_v<T>) {
        for (std::size_t i = 0; i < n; ++i) out[i] = a[i];
    } else {
        const hwy::HWY_NAMESPACE::ScalableTag<T> d;
        const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
        std::size_t i = 0;
        for (; i + lanes <= n; i += lanes)
            hwy::HWY_NAMESPACE::StoreU(
                hwy::HWY_NAMESPACE::Abs(hwy::HWY_NAMESPACE::LoadU(d, a + i)), d,
                out + i);
        for (; i < n; ++i) out[i] = a[i] < 0 ? static_cast<T>(-a[i]) : a[i];
    }
}

// A clip bound read as `T`. On an integer column an infinite, NaN or
// out-of-range bound saturates (a NaN bound clips nothing) where a plain cast
// would be undefined.
template <class T>
T clip_bound(dftu_scalar s, bool upper) {
    if constexpr (std::is_integral_v<T>) {
        constexpr T LOW = std::numeric_limits<T>::lowest();
        constexpr T HIGH = std::numeric_limits<T>::max();
        switch (s.kind) {
            case DFTU_SCALAR_TAG_I64:
                if (s.value.i < 0 && std::is_unsigned_v<T>) return LOW;
                if (s.value.i >= 0 && static_cast<std::uint64_t>(s.value.i) >
                                          static_cast<std::uint64_t>(HIGH))
                    return HIGH;
                if (s.value.i < 0 && std::is_signed_v<T> &&
                    s.value.i < static_cast<std::int64_t>(LOW))
                    return LOW;
                return static_cast<T>(s.value.i);
            case DFTU_SCALAR_TAG_U64:
                return s.value.u > static_cast<std::uint64_t>(HIGH)
                           ? HIGH
                           : static_cast<T>(s.value.u);
            case DFTU_SCALAR_TAG_STR:
            case DFTU_SCALAR_TAG_ERR:
                return upper ? HIGH : LOW;
            default: {
                const double d = s.value.d;
                if (d != d) return upper ? HIGH : LOW;
                if (d >= static_cast<double>(HIGH)) return HIGH;
                if (d <= static_cast<double>(LOW)) return LOW;
                return static_cast<T>(d);
            }
        }
    } else {
        return scalar_value<T>(s);
    }
}

template <class T>
void ClipImpl(const void* av, dftu_scalar lo, dftu_scalar hi, void* ov,
              std::size_t n) {
    const T* a = static_cast<const T*>(av);
    T* out = static_cast<T*>(ov);
    T l = clip_bound<T>(lo, false);
    T h = clip_bound<T>(hi, true);
    // A NaN bound clips nothing and a NaN value stays NaN (Polars, pandas).
    if constexpr (std::is_floating_point_v<T>) {
        if (l != l) l = -std::numeric_limits<T>::infinity();
        if (h != h) h = std::numeric_limits<T>::infinity();
    }
    const hwy::HWY_NAMESPACE::ScalableTag<T> d;
    const auto vl = hwy::HWY_NAMESPACE::Set(d, l);
    const auto vh = hwy::HWY_NAMESPACE::Set(d, h);
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes) {
        const auto v = hwy::HWY_NAMESPACE::LoadU(d, a + i);
        auto r = hwy::HWY_NAMESPACE::Min(hwy::HWY_NAMESPACE::Max(v, vl), vh);
        if constexpr (std::is_floating_point_v<T>)
            r = hwy::HWY_NAMESPACE::IfThenElse(hwy::HWY_NAMESPACE::IsNaN(v), v,
                                               r);
        hwy::HWY_NAMESPACE::StoreU(r, d, out + i);
    }
    for (; i < n; ++i) out[i] = a[i] < l ? l : (a[i] > h ? h : a[i]);
}

template <class T>
void RoundImpl(const void* av, void* ov, std::size_t n) {
    const T* a = static_cast<const T*>(av);
    T* out = static_cast<T*>(ov);
    if constexpr (std::is_floating_point_v<T>) {
        const hwy::HWY_NAMESPACE::ScalableTag<T> d;
        const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
        std::size_t i = 0;
        for (; i + lanes <= n; i += lanes)
            hwy::HWY_NAMESPACE::StoreU(
                hwy::HWY_NAMESPACE::Round(hwy::HWY_NAMESPACE::LoadU(d, a + i)),
                d, out + i);
        for (; i < n; ++i) out[i] = static_cast<T>(std::nearbyint(a[i]));
    } else {
        for (std::size_t i = 0; i < n; ++i) out[i] = a[i];
    }
}

// ceil/floor/trunc mirror RoundImpl: SIMD on floats, integers copied.
template <class T, int MODE>
void RoundModeImpl(const void* av, void* ov, std::size_t n) {
    const T* a = static_cast<const T*>(av);
    T* out = static_cast<T*>(ov);
    if constexpr (std::is_floating_point_v<T>) {
        const hwy::HWY_NAMESPACE::ScalableTag<T> d;
        const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
        std::size_t i = 0;
        for (; i + lanes <= n; i += lanes) {
            const auto v = hwy::HWY_NAMESPACE::LoadU(d, a + i);
            const auto r = MODE == 0   ? hwy::HWY_NAMESPACE::Ceil(v)
                           : MODE == 1 ? hwy::HWY_NAMESPACE::Floor(v)
                                       : hwy::HWY_NAMESPACE::Trunc(v);
            hwy::HWY_NAMESPACE::StoreU(r, d, out + i);
        }
        for (; i < n; ++i)
            out[i] = static_cast<T>(MODE == 0   ? std::ceil(a[i])
                                    : MODE == 1 ? std::floor(a[i])
                                                : std::trunc(a[i]));
    } else {
        for (std::size_t i = 0; i < n; ++i) out[i] = a[i];
    }
}
template <class T>
void CeilImpl(const void* a, void* o, std::size_t n) {
    RoundModeImpl<T, 0>(a, o, n);
}
template <class T>
void FloorImpl(const void* a, void* o, std::size_t n) {
    RoundModeImpl<T, 1>(a, o, n);
}
template <class T>
void TruncImpl(const void* a, void* o, std::size_t n) {
    RoundModeImpl<T, 2>(a, o, n);
}

template <class T>
void SignImpl(const void* av, void* ov, std::size_t n) {
    const T* a = static_cast<const T*>(av);
    T* out = static_cast<T*>(ov);
    const hwy::HWY_NAMESPACE::ScalableTag<T> d;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    const auto zero = hwy::HWY_NAMESPACE::Zero(d);
    const auto one = hwy::HWY_NAMESPACE::Set(d, T{1});
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes) {
        const auto v = hwy::HWY_NAMESPACE::LoadU(d, a + i);
        auto r = hwy::HWY_NAMESPACE::IfThenElseZero(
            hwy::HWY_NAMESPACE::Gt(v, zero), one);
        if constexpr (!std::is_unsigned_v<T>)
            r = hwy::HWY_NAMESPACE::Sub(
                r, hwy::HWY_NAMESPACE::IfThenElseZero(
                       hwy::HWY_NAMESPACE::Lt(v, zero), one));
        if constexpr (std::is_floating_point_v<T>)
            r = hwy::HWY_NAMESPACE::IfThenElse(hwy::HWY_NAMESPACE::IsNaN(v), v,
                                               r);
        hwy::HWY_NAMESPACE::StoreU(r, d, out + i);
    }
    for (; i < n; ++i) {
        if constexpr (std::is_unsigned_v<T>)
            out[i] = a[i] > 0 ? T{1} : T{0};
        else if constexpr (std::is_floating_point_v<T>)
            out[i] = a[i] != a[i]
                         ? a[i]
                         : (a[i] > 0 ? T{1} : (a[i] < 0 ? T{-1} : T{0}));
        else
            out[i] = a[i] > 0 ? T{1} : (a[i] < 0 ? static_cast<T>(-1) : T{0});
    }
}

template <class T>
void NegateImpl(const void* av, void* ov, std::size_t n) {
    const T* a = static_cast<const T*>(av);
    T* out = static_cast<T*>(ov);
    if constexpr (std::is_unsigned_v<T>) {
        for (std::size_t i = 0; i < n; ++i)
            out[i] = static_cast<T>(T{0} - a[i]);
    } else {
        const hwy::HWY_NAMESPACE::ScalableTag<T> d;
        const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
        std::size_t i = 0;
        for (; i + lanes <= n; i += lanes)
            hwy::HWY_NAMESPACE::StoreU(
                hwy::HWY_NAMESPACE::Neg(hwy::HWY_NAMESPACE::LoadU(d, a + i)), d,
                out + i);
        for (; i < n; ++i) out[i] = static_cast<T>(-a[i]);
    }
}

// In-place SIMD sqrt over a Float64 buffer (input already widened to double).
void SqrtF64Impl(double* p, std::size_t n) {
    const hwy::HWY_NAMESPACE::ScalableTag<double> d;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes)
        hwy::HWY_NAMESPACE::StoreU(
            hwy::HWY_NAMESPACE::Sqrt(hwy::HWY_NAMESPACE::LoadU(d, p + i)), d,
            p + i);
    for (; i < n; ++i) p[i] = std::sqrt(p[i]);
}

// In-place SIMD exp/log over a Float64 buffer (input already widened to
// double). Highway's Exp/Log are accurate to <= 4 ULP versus libm, but only
// on their valid range (exp: [-DBL_MAX, 706], log: (0, DBL_MAX]); outside it,
// and on NaN, they return garbage. A vector with any lane outside a range that
// is safe for both is computed with libm, so 0, negatives, infinities, NaN and
// overflow follow IEEE.
void ExpF64Impl(double* p, std::size_t n) {
    const hwy::HWY_NAMESPACE::ScalableTag<double> d;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    const auto lo = hwy::HWY_NAMESPACE::Set(d, -700.0);
    const auto hi = hwy::HWY_NAMESPACE::Set(d, 700.0);
    // Four vectors share one all-lanes test, so the horizontal reduction runs
    // a quarter as often.
    std::size_t i = 0;
    for (; i + 4 * lanes <= n; i += 4 * lanes) {
        const auto x0 = hwy::HWY_NAMESPACE::LoadU(d, p + i);
        const auto x1 = hwy::HWY_NAMESPACE::LoadU(d, p + i + lanes);
        const auto x2 = hwy::HWY_NAMESPACE::LoadU(d, p + i + 2 * lanes);
        const auto x3 = hwy::HWY_NAMESPACE::LoadU(d, p + i + 3 * lanes);
        const auto ok = hwy::HWY_NAMESPACE::And(
            hwy::HWY_NAMESPACE::And(
                hwy::HWY_NAMESPACE::And(hwy::HWY_NAMESPACE::Ge(x0, lo),
                                        hwy::HWY_NAMESPACE::Le(x0, hi)),
                hwy::HWY_NAMESPACE::And(hwy::HWY_NAMESPACE::Ge(x1, lo),
                                        hwy::HWY_NAMESPACE::Le(x1, hi))),
            hwy::HWY_NAMESPACE::And(
                hwy::HWY_NAMESPACE::And(hwy::HWY_NAMESPACE::Ge(x2, lo),
                                        hwy::HWY_NAMESPACE::Le(x2, hi)),
                hwy::HWY_NAMESPACE::And(hwy::HWY_NAMESPACE::Ge(x3, lo),
                                        hwy::HWY_NAMESPACE::Le(x3, hi))));
        if (hwy::HWY_NAMESPACE::AllTrue(d, ok)) {
            hwy::HWY_NAMESPACE::StoreU(hwy::HWY_NAMESPACE::Exp(d, x0), d,
                                       p + i);
            hwy::HWY_NAMESPACE::StoreU(hwy::HWY_NAMESPACE::Exp(d, x1), d,
                                       p + i + lanes);
            hwy::HWY_NAMESPACE::StoreU(hwy::HWY_NAMESPACE::Exp(d, x2), d,
                                       p + i + 2 * lanes);
            hwy::HWY_NAMESPACE::StoreU(hwy::HWY_NAMESPACE::Exp(d, x3), d,
                                       p + i + 3 * lanes);
        } else {
            // A bad value spoils only its own vector.
            for (std::size_t v = 0; v < 4; ++v) {
                const std::size_t at = i + v * lanes;
                const auto x = hwy::HWY_NAMESPACE::LoadU(d, p + at);
                if (hwy::HWY_NAMESPACE::AllTrue(
                        d, hwy::HWY_NAMESPACE::And(
                               hwy::HWY_NAMESPACE::Ge(x, lo),
                               hwy::HWY_NAMESPACE::Le(x, hi)))) {
                    hwy::HWY_NAMESPACE::StoreU(hwy::HWY_NAMESPACE::Exp(d, x), d,
                                               p + at);
                } else {
                    for (std::size_t j = at; j < at + lanes; ++j)
                        p[j] = std::exp(p[j]);
                }
            }
        }
    }
    for (; i < n; ++i) p[i] = std::exp(p[i]);
}

void LogF64Impl(double* p, std::size_t n) {
    const hwy::HWY_NAMESPACE::ScalableTag<double> d;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    const auto zero = hwy::HWY_NAMESPACE::Zero(d);
    const auto max =
        hwy::HWY_NAMESPACE::Set(d, std::numeric_limits<double>::max());
    std::size_t i = 0;
    for (; i + 4 * lanes <= n; i += 4 * lanes) {
        const auto x0 = hwy::HWY_NAMESPACE::LoadU(d, p + i);
        const auto x1 = hwy::HWY_NAMESPACE::LoadU(d, p + i + lanes);
        const auto x2 = hwy::HWY_NAMESPACE::LoadU(d, p + i + 2 * lanes);
        const auto x3 = hwy::HWY_NAMESPACE::LoadU(d, p + i + 3 * lanes);
        const auto ok = hwy::HWY_NAMESPACE::And(
            hwy::HWY_NAMESPACE::And(
                hwy::HWY_NAMESPACE::And(hwy::HWY_NAMESPACE::Gt(x0, zero),
                                        hwy::HWY_NAMESPACE::Le(x0, max)),
                hwy::HWY_NAMESPACE::And(hwy::HWY_NAMESPACE::Gt(x1, zero),
                                        hwy::HWY_NAMESPACE::Le(x1, max))),
            hwy::HWY_NAMESPACE::And(
                hwy::HWY_NAMESPACE::And(hwy::HWY_NAMESPACE::Gt(x2, zero),
                                        hwy::HWY_NAMESPACE::Le(x2, max)),
                hwy::HWY_NAMESPACE::And(hwy::HWY_NAMESPACE::Gt(x3, zero),
                                        hwy::HWY_NAMESPACE::Le(x3, max))));
        if (hwy::HWY_NAMESPACE::AllTrue(d, ok)) {
            hwy::HWY_NAMESPACE::StoreU(hwy::HWY_NAMESPACE::Log(d, x0), d,
                                       p + i);
            hwy::HWY_NAMESPACE::StoreU(hwy::HWY_NAMESPACE::Log(d, x1), d,
                                       p + i + lanes);
            hwy::HWY_NAMESPACE::StoreU(hwy::HWY_NAMESPACE::Log(d, x2), d,
                                       p + i + 2 * lanes);
            hwy::HWY_NAMESPACE::StoreU(hwy::HWY_NAMESPACE::Log(d, x3), d,
                                       p + i + 3 * lanes);
        } else {
            for (std::size_t v = 0; v < 4; ++v) {
                const std::size_t at = i + v * lanes;
                const auto x = hwy::HWY_NAMESPACE::LoadU(d, p + at);
                if (hwy::HWY_NAMESPACE::AllTrue(
                        d, hwy::HWY_NAMESPACE::And(
                               hwy::HWY_NAMESPACE::Gt(x, zero),
                               hwy::HWY_NAMESPACE::Le(x, max)))) {
                    hwy::HWY_NAMESPACE::StoreU(hwy::HWY_NAMESPACE::Log(d, x), d,
                                               p + at);
                } else {
                    for (std::size_t j = at; j < at + lanes; ++j)
                        p[j] = std::log(p[j]);
                }
            }
        }
    }
    for (; i < n; ++i) p[i] = std::log(p[i]);
}

void AbsKernel(std::int32_t type, const void* a, void* out, std::size_t n) {
    DFTRACER_UTILS_NUMERIC_DISPATCH(static_cast<TypeId>(type), AbsImpl, a, out,
                                    n)
}
void CeilKernel(std::int32_t type, const void* a, void* out, std::size_t n) {
    DFTRACER_UTILS_NUMERIC_DISPATCH(static_cast<TypeId>(type), CeilImpl, a, out,
                                    n)
}
void FloorKernel(std::int32_t type, const void* a, void* out, std::size_t n) {
    DFTRACER_UTILS_NUMERIC_DISPATCH(static_cast<TypeId>(type), FloorImpl, a,
                                    out, n)
}
void TruncKernel(std::int32_t type, const void* a, void* out, std::size_t n) {
    DFTRACER_UTILS_NUMERIC_DISPATCH(static_cast<TypeId>(type), TruncImpl, a,
                                    out, n)
}
void SignKernel(std::int32_t type, const void* a, void* out, std::size_t n) {
    DFTRACER_UTILS_NUMERIC_DISPATCH(static_cast<TypeId>(type), SignImpl, a, out,
                                    n)
}
void NegateKernel(std::int32_t type, const void* a, void* out, std::size_t n) {
    DFTRACER_UTILS_NUMERIC_DISPATCH(static_cast<TypeId>(type), NegateImpl, a,
                                    out, n)
}
void SqrtKernel(double* p, std::size_t n) { SqrtF64Impl(p, n); }
void ExpKernel(double* p, std::size_t n) { ExpF64Impl(p, n); }
void LogKernel(double* p, std::size_t n) { LogF64Impl(p, n); }
void ClipKernel(std::int32_t type, const void* a, dftu_scalar lo,
                dftu_scalar hi, void* out, std::size_t n) {
    DFTRACER_UTILS_NUMERIC_DISPATCH(static_cast<TypeId>(type), ClipImpl, a, lo,
                                    hi, out, n)
}
void RoundKernel(std::int32_t type, const void* a, void* out, std::size_t n) {
    DFTRACER_UTILS_NUMERIC_DISPATCH(static_cast<TypeId>(type), RoundImpl, a,
                                    out, n)
}

// fillna: out[i] = valid(i) ? in[i] : fill. No validity means all-valid, a
// plain copy. Otherwise the packed validity bits drive a blend: 64 rows a word
// at a time, each lanes-wide chunk shifted to bit 0 so LoadMaskBits reads it
// aligned.
template <class T>
void FillImpl(const void* av, const std::uint8_t* valid, dftu_scalar s,
              void* ov, std::size_t n) {
    const T* in = static_cast<const T*>(av);
    T* out = static_cast<T*>(ov);
    const T fv = scalar_value<T>(s);
    if (!valid) {
        if (n) std::memcpy(out, in, n * sizeof(T));
        return;
    }
    const hwy::HWY_NAMESPACE::ScalableTag<T> d;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    const auto vfill = hwy::HWY_NAMESPACE::Set(d, fv);
    std::size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        std::uint64_t w;
        std::memcpy(&w, valid + (i >> 3), 8);
        for (std::size_t c = 0; c < 64; c += lanes) {
            std::uint64_t sub = w >> c;
            const auto m = hwy::HWY_NAMESPACE::LoadMaskBits(
                d, reinterpret_cast<std::uint8_t*>(&sub));
            hwy::HWY_NAMESPACE::StoreU(
                hwy::HWY_NAMESPACE::IfThenElse(
                    m, hwy::HWY_NAMESPACE::LoadU(d, in + i + c), vfill),
                d, out + i + c);
        }
    }
    for (; i < n; ++i) out[i] = ((valid[i >> 3] >> (i & 7)) & 1) ? in[i] : fv;
}

// One log-step of an inclusive in-vector scan: fold in the vector shifted up by
// S lanes, with the vacated low S lanes filled with the op identity. S must be
// a compile-time distance, so the caller recurses over 1,2,4,...; the guard
// stops once S covers the widest possible vector for this target.
template <int S, class D, class V, class OpF>
HWY_INLINE void scan_step(D d, V& v, OpF op, V id) {
    if constexpr (S < HWY_MAX_LANES_D(D)) {
        const V sh = hwy::HWY_NAMESPACE::IfThenElse(
            hwy::HWY_NAMESPACE::FirstN(d, S), id,
            hwy::HWY_NAMESPACE::ShiftLeftLanes<S>(d, v));
        v = op(v, sh);
    }
}

template <class D, class V, class OpF>
HWY_INLINE V inclusive_scan(D d, V v, OpF op, V id) {
    scan_step<1>(d, v, op, id);
    scan_step<2>(d, v, op, id);
    scan_step<4>(d, v, op, id);
    scan_step<8>(d, v, op, id);
    scan_step<16>(d, v, op, id);
    scan_step<32>(d, v, op, id);
    return v;
}

// Prefix scan over a null-free column: scan each block in-vector, then fold the
// running carry (the previous block's last lane) into every lane. The op is
// associative, so integer sum/product and every min/max match the scalar result
// exactly; only float sum/product reassociate (ULP-level difference).
// The log-step in-vector scan only pays off on wide vectors (its per-block
// horizontal shifts + ExtractLane are fixed overhead); on narrow targets a
// plain scalar carry wins. On a fixed-width target Lanes(d) is a compile-time
// constant so this gate folds away and the dead branch is eliminated per
// target.
constexpr std::size_t SCAN_SIMD_MIN_LANES = 8;

template <class T>
void CumSumImpl(const void* av, void* ov, std::size_t n) {
    const T* in = static_cast<const T*>(av);
    T* out = static_cast<T*>(ov);
    // Cap the scan vector to 128 bits: ShiftLeftLanes is a per-128-bit-block
    // shift on x86, so a wider in-vector scan is silently wrong on AVX2 and
    // fails to compile on AVX-512 (ShiftLeftBytes asserts kBytes <= 16). The
    // per-chunk carry loop below stitches the 128-bit chunks together.
    const hwy::HWY_NAMESPACE::CappedTag<T, 16 / sizeof(T)> d;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    if (lanes < SCAN_SIMD_MIN_LANES) {
        T carry = T{0};
        for (std::size_t i = 0; i < n; ++i)
            out[i] = carry = static_cast<T>(carry + in[i]);
        return;
    }
    const auto add = [](auto a, auto b) {
        return hwy::HWY_NAMESPACE::Add(a, b);
    };
    const auto vid = hwy::HWY_NAMESPACE::Zero(d);
    T carry = T{0};
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes) {
        auto v =
            inclusive_scan(d, hwy::HWY_NAMESPACE::LoadU(d, in + i), add, vid);
        v = hwy::HWY_NAMESPACE::Add(v, hwy::HWY_NAMESPACE::Set(d, carry));
        hwy::HWY_NAMESPACE::StoreU(v, d, out + i);
        carry = hwy::HWY_NAMESPACE::ExtractLane(v, lanes - 1);
    }
    for (; i < n; ++i) out[i] = carry = static_cast<T>(carry + in[i]);
}

template <class T>
void CumProdImpl(const void* av, void* ov, std::size_t n) {
    const T* in = static_cast<const T*>(av);
    T* out = static_cast<T*>(ov);
    // 128-bit cap: see CumSumImpl (ShiftLeftLanes is per-128-bit-block).
    const hwy::HWY_NAMESPACE::CappedTag<T, 16 / sizeof(T)> d;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    if (lanes < SCAN_SIMD_MIN_LANES) {
        T carry = T{1};
        for (std::size_t i = 0; i < n; ++i)
            out[i] = carry = static_cast<T>(carry * in[i]);
        return;
    }
    const auto mul = [](auto a, auto b) {
        return hwy::HWY_NAMESPACE::Mul(a, b);
    };
    const auto vid = hwy::HWY_NAMESPACE::Set(d, T{1});
    T carry = T{1};
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes) {
        auto v =
            inclusive_scan(d, hwy::HWY_NAMESPACE::LoadU(d, in + i), mul, vid);
        v = hwy::HWY_NAMESPACE::Mul(v, hwy::HWY_NAMESPACE::Set(d, carry));
        hwy::HWY_NAMESPACE::StoreU(v, d, out + i);
        carry = hwy::HWY_NAMESPACE::ExtractLane(v, lanes - 1);
    }
    for (; i < n; ++i) out[i] = carry = static_cast<T>(carry * in[i]);
}

template <class T, bool IsMax>
void CumExtremeImpl(const void* av, void* ov, std::size_t n) {
    const T* in = static_cast<const T*>(av);
    T* out = static_cast<T*>(ov);
    // 128-bit cap: see CumSumImpl (ShiftLeftLanes is per-128-bit-block).
    const hwy::HWY_NAMESPACE::CappedTag<T, 16 / sizeof(T)> d;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d);
    const T ident = IsMax ? std::numeric_limits<T>::lowest()
                          : std::numeric_limits<T>::max();
    if (lanes < SCAN_SIMD_MIN_LANES) {
        T carry = ident;
        for (std::size_t i = 0; i < n; ++i) {
            carry = IsMax ? (in[i] > carry ? in[i] : carry)
                          : (in[i] < carry ? in[i] : carry);
            out[i] = carry;
        }
        return;
    }
    const auto op = [](auto a, auto b) {
        return IsMax ? hwy::HWY_NAMESPACE::Max(a, b)
                     : hwy::HWY_NAMESPACE::Min(a, b);
    };
    const auto vid = hwy::HWY_NAMESPACE::Set(d, ident);
    T carry = ident;
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes) {
        auto v =
            inclusive_scan(d, hwy::HWY_NAMESPACE::LoadU(d, in + i), op, vid);
        v = IsMax
                ? hwy::HWY_NAMESPACE::Max(v, hwy::HWY_NAMESPACE::Set(d, carry))
                : hwy::HWY_NAMESPACE::Min(v, hwy::HWY_NAMESPACE::Set(d, carry));
        hwy::HWY_NAMESPACE::StoreU(v, d, out + i);
        carry = hwy::HWY_NAMESPACE::ExtractLane(v, lanes - 1);
    }
    for (; i < n; ++i) {
        carry = IsMax ? (in[i] > carry ? in[i] : carry)
                      : (in[i] < carry ? in[i] : carry);
        out[i] = carry;
    }
}
template <class T>
void CumMaxImpl(const void* av, void* ov, std::size_t n) {
    CumExtremeImpl<T, true>(av, ov, n);
}
template <class T>
void CumMinImpl(const void* av, void* ov, std::size_t n) {
    CumExtremeImpl<T, false>(av, ov, n);
}

// Chunk size for the two-pass parallel prefix scan below; small enough to
// give several chunks per core on a 20M-row column, large enough that the
// per-chunk fan-out cost stays negligible.
constexpr std::int64_t CUM_SCAN_GRAIN = 1 << 20;

template <class T>
void CumSumParallel(const void* av, void* ov, std::size_t n) {
    const T* in = static_cast<const T*>(av);
    T* out = static_cast<T*>(ov);
    parallel_prefix_scan<T>(
        static_cast<std::int64_t>(n), CUM_SCAN_GRAIN, T{0},
        [&](std::int64_t b, std::int64_t e) -> T {
            CumSumImpl<T>(in + b, out + b, static_cast<std::size_t>(e - b));
            return out[e - 1];
        },
        [&](std::int64_t b, std::int64_t e, T off) {
            for (std::int64_t i = b; i < e; ++i)
                out[i] = static_cast<T>(off + out[i]);
        },
        [](T a, T b) { return static_cast<T>(a + b); });
}

template <class T>
void CumProdParallel(const void* av, void* ov, std::size_t n) {
    const T* in = static_cast<const T*>(av);
    T* out = static_cast<T*>(ov);
    parallel_prefix_scan<T>(
        static_cast<std::int64_t>(n), CUM_SCAN_GRAIN, T{1},
        [&](std::int64_t b, std::int64_t e) -> T {
            CumProdImpl<T>(in + b, out + b, static_cast<std::size_t>(e - b));
            return out[e - 1];
        },
        [&](std::int64_t b, std::int64_t e, T off) {
            for (std::int64_t i = b; i < e; ++i)
                out[i] = static_cast<T>(off * out[i]);
        },
        [](T a, T b) { return static_cast<T>(a * b); });
}

template <class T, bool IsMax>
void CumExtremeParallel(const void* av, void* ov, std::size_t n) {
    const T* in = static_cast<const T*>(av);
    T* out = static_cast<T*>(ov);
    const T ident = IsMax ? std::numeric_limits<T>::lowest()
                          : std::numeric_limits<T>::max();
    parallel_prefix_scan<T>(
        static_cast<std::int64_t>(n), CUM_SCAN_GRAIN, ident,
        [&](std::int64_t b, std::int64_t e) -> T {
            CumExtremeImpl<T, IsMax>(in + b, out + b,
                                     static_cast<std::size_t>(e - b));
            return out[e - 1];
        },
        [&](std::int64_t b, std::int64_t e, T off) {
            for (std::int64_t i = b; i < e; ++i)
                out[i] = IsMax ? (off > out[i] ? off : out[i])
                               : (off < out[i] ? off : out[i]);
        },
        [](T a, T b) { return IsMax ? (a > b ? a : b) : (a < b ? a : b); });
}
template <class T>
void CumMaxParallel(const void* av, void* ov, std::size_t n) {
    CumExtremeParallel<T, true>(av, ov, n);
}
template <class T>
void CumMinParallel(const void* av, void* ov, std::size_t n) {
    CumExtremeParallel<T, false>(av, ov, n);
}

void CumSumKernel(std::int32_t type, const void* a, void* out, std::size_t n) {
    DFTRACER_UTILS_NUMERIC_DISPATCH(static_cast<TypeId>(type), CumSumParallel,
                                    a, out, n)
}
void CumProdKernel(std::int32_t type, const void* a, void* out, std::size_t n) {
    DFTRACER_UTILS_NUMERIC_DISPATCH(static_cast<TypeId>(type), CumProdParallel,
                                    a, out, n)
}
void CumMaxKernel(std::int32_t type, const void* a, void* out, std::size_t n) {
    DFTRACER_UTILS_NUMERIC_DISPATCH(static_cast<TypeId>(type), CumMaxParallel,
                                    a, out, n)
}
void CumMinKernel(std::int32_t type, const void* a, void* out, std::size_t n) {
    DFTRACER_UTILS_NUMERIC_DISPATCH(static_cast<TypeId>(type), CumMinParallel,
                                    a, out, n)
}

void FillKernel(std::int32_t type, const void* a, const std::uint8_t* valid,
                dftu_scalar s, void* out, std::size_t n) {
    DFTRACER_UTILS_NUMERIC_DISPATCH(static_cast<TypeId>(type), FillImpl, a,
                                    valid, s, out, n)
}

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::dataframe
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::dataframe {

HWY_EXPORT(AbsKernel);
HWY_EXPORT(ClipKernel);
HWY_EXPORT(RoundKernel);
HWY_EXPORT(CeilKernel);
HWY_EXPORT(FloorKernel);
HWY_EXPORT(TruncKernel);
HWY_EXPORT(SignKernel);
HWY_EXPORT(NegateKernel);
HWY_EXPORT(SqrtKernel);
HWY_EXPORT(ExpKernel);
HWY_EXPORT(LogKernel);
HWY_EXPORT(FillKernel);
HWY_EXPORT(CumSumKernel);
HWY_EXPORT(CumProdKernel);
HWY_EXPORT(CumMaxKernel);
HWY_EXPORT(CumMinKernel);

namespace {

// Exactly the set the kernels below have a lane type for. Float16, the
// decimals and every byte or nested type reach no case in the dispatch, so
// letting them through would leave the output buffer untouched.
bool is_numeric(TypeId t) { return is_numeric_dispatchable(t); }

bool is_valid(const dftu_series& v, std::int64_t i) {
    if (!v.validity.block()) return true;
    return (v.validity.data()[i >> 3] & (1u << (i & 7))) != 0;
}

dftu_series* alloc_like(const dftu_series* a, bool keep_validity) {
    auto* out = new dftu_series();
    out->type = a->type;
    out->encoding = Encoding::Flat;
    out->length = a->length;
    out->null_count = keep_validity ? a->null_count : 0;
    out->validity = keep_validity ? a->validity : BufferView{};
    out->data = new_buffer(buffer_bytes(a->type, a->length));
    return out;
}

// fillna over a Bool column: the packed bits are copied and each null row takes
// the fill's truth value (an integer or float fill is true unless zero).
dftu_series* fillna_bool(const dftu_series* a, dftu_scalar fill) {
    bool fv = false;
    switch (fill.kind) {
        case DFTU_SCALAR_TAG_I64:
            fv = fill.value.i != 0;
            break;
        case DFTU_SCALAR_TAG_U64:
            fv = fill.value.u != 0;
            break;
        case DFTU_SCALAR_TAG_F64:
            fv = fill.value.d != 0.0;
            break;
        default:
            return nullptr;
    }
    dftu_series* out = alloc_like(a, false);
    auto* bits = static_cast<std::uint8_t*>(writable(out->data));
    std::memcpy(bits, a->data.data(), out->data.size());
    if (a->validity.block()) {
        const auto* valid = a->validity.data();
        for (std::int64_t i = 0; i < a->length; ++i) {
            if ((valid[i >> 3] >> (i & 7)) & 1) continue;
            if (fv)
                bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
            else
                bits[i >> 3] &= static_cast<std::uint8_t>(~(1u << (i & 7)));
        }
    }
    return out;
}

// fillna over a String column: each null row becomes the fill text. Only the
// narrow-offset String type is handled; LargeString returns null.
dftu_series* fillna_string(const dftu_series* a, dftu_scalar fill) {
    if (fill.kind != DFTU_SCALAR_TAG_STR || a->type != TypeId::String)
        return nullptr;
    const dstd::StringView fv(fill.value.s, fill.len);
    const auto* off = reinterpret_cast<const std::int32_t*>(
        offsets_of<std::int32_t>(*a).data());
    const char* text = reinterpret_cast<const char*>(a->data.data());
    const std::uint8_t* valid =
        a->validity.block() ? a->validity.data() : nullptr;
    dstd::Vector<dstd::StringView> views(static_cast<std::size_t>(a->length));
    for (std::int64_t i = 0; i < a->length; ++i)
        views[static_cast<std::size_t>(i)] =
            (!valid || ((valid[i >> 3] >> (i & 7)) & 1))
                ? dstd::StringView(text + off[i], static_cast<std::size_t>(
                                                      off[i + 1] - off[i]))
                : fv;
    return Series::strings(dstd::Span<const dstd::StringView>(views), nullptr)
        .release();
}

// cumsum and the running extrema are scalar (sequential prefix scans); one
// template each, dispatched by type. Nulls make the per-row "seen" state
// path-dependent in a way that is not worth the risk to parallelize (this is
// only the has-nulls fallback; the hot dense path is parallelized above).
template <class T>
void cumsum_one(const dftu_series& a, void* ov) {
    T* out = static_cast<T*>(ov);
    const T* in = reinterpret_cast<const T*>(a.data.data());
    T acc = T{0};
    for (std::int64_t i = 0; i < a.length; ++i) {
        if (is_valid(a, i)) acc = static_cast<T>(acc + in[i]);
        out[i] = acc;
    }
}

// Running max/min: seed from the first valid value; a null row's slot holds
// the carried acc (the output keeps the input's validity, so it reads null).
template <class T, bool IsMax>
void cumextreme_one(const dftu_series& a, void* ov) {
    T* out = static_cast<T*>(ov);
    const T* in = reinterpret_cast<const T*>(a.data.data());
    T acc = T{0};
    bool seen = false;
    for (std::int64_t i = 0; i < a.length; ++i) {
        if (is_valid(a, i)) {
            if (!seen) {
                acc = in[i];
                seen = true;
            } else if (IsMax) {
                if (in[i] > acc) acc = in[i];
            } else if (in[i] < acc) {
                acc = in[i];
            }
        }
        out[i] = acc;
    }
}
template <class T>
void cummax_one(const dftu_series& a, void* ov) {
    cumextreme_one<T, true>(a, ov);
}
template <class T>
void cummin_one(const dftu_series& a, void* ov) {
    cumextreme_one<T, false>(a, ov);
}

// Running product; nulls act as the identity (1), matching cumsum's null skip.
template <class T>
void cumprod_one(const dftu_series& a, void* ov) {
    T* out = static_cast<T*>(ov);
    const T* in = reinterpret_cast<const T*>(a.data.data());
    T acc = T{1};
    for (std::int64_t i = 0; i < a.length; ++i) {
        if (is_valid(a, i)) acc = static_cast<T>(acc * in[i]);
        out[i] = acc;
    }
}

// diff / pct_change emit their own validity bitmap: row 0 is null, and any row
// whose value or predecessor is null is null too.
void set_valid(std::uint8_t* v, std::int64_t i) {
    v[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
}

template <class T>
void diff_one(const dftu_series& a, void* ov, std::uint8_t* valid) {
    T* out = static_cast<T*>(ov);
    const T* in = reinterpret_cast<const T*>(a.data.data());
    for (std::int64_t i = 0; i < a.length; ++i) {
        out[i] = T{0};
        if (i == 0 || !is_valid(a, i) || !is_valid(a, i - 1)) continue;
        out[i] = static_cast<T>(in[i] - in[i - 1]);
        set_valid(valid, i);
    }
}

template <class T>
void pctchange_one(const dftu_series& a, double* out, std::uint8_t* valid) {
    const T* in = reinterpret_cast<const T*>(a.data.data());
    for (std::int64_t i = 0; i < a.length; ++i) {
        out[i] = 0.0;
        if (i == 0 || !is_valid(a, i) || !is_valid(a, i - 1)) continue;
        out[i] = (static_cast<double>(in[i]) - static_cast<double>(in[i - 1])) /
                 static_cast<double>(in[i - 1]);
        set_valid(valid, i);
    }
}

// Null-free data-only variants: no per-row validity branch, so the compiler
// auto-vectorizes the subtract/divide. The caller fills validity in bulk (all
// rows valid except row 0). This is what makes diff/pct fast on a dense column.
template <class T>
void diff_data_one(const dftu_series& a, void* ov) {
    T* out = static_cast<T*>(ov);
    const T* in = reinterpret_cast<const T*>(a.data.data());
    if (a.length > 0) out[0] = T{0};
    for (std::int64_t i = 1; i < a.length; ++i)
        out[i] = static_cast<T>(in[i] - in[i - 1]);
}

template <class T>
void pctchange_data_one(const dftu_series& a, double* out) {
    const T* in = reinterpret_cast<const T*>(a.data.data());
    if (a.length > 0) out[0] = 0.0;
    for (std::int64_t i = 1; i < a.length; ++i)
        out[i] = (static_cast<double>(in[i]) - static_cast<double>(in[i - 1])) /
                 static_cast<double>(in[i - 1]);
}

// Mark every row valid except row 0 (diff/pct's only structural null on a
// dense column), a byte fill instead of a per-row bit set. Trailing bits past
// length are cleared so the buffer stays canonical.
void fill_valid_except_first(std::uint8_t* v, std::int64_t n) {
    if (n <= 0) return;
    const std::size_t nbytes = static_cast<std::size_t>((n + 7) / 8);
    std::memset(v, 0xFF, nbytes);
    v[0] &= static_cast<std::uint8_t>(~1u);  // row 0 is null
    const std::size_t tail = static_cast<std::size_t>(n & 7);
    if (tail) v[nbytes - 1] &= static_cast<std::uint8_t>((1u << tail) - 1);
}

// Widen any numeric column to a double buffer (scalar cast; nulls read as-is).
template <class T>
void widen_f64(const dftu_series& a, double* out) {
    const T* in = reinterpret_cast<const T*>(a.data.data());
    for (std::int64_t i = 0; i < a.length; ++i)
        out[i] = static_cast<double>(in[i]);
}

// A fresh zeroed validity bitmap; the caller marks valid rows, then
// attach_bitmap wires it in and computes null_count.
BufferView new_bitmap(std::int64_t n) {
    auto bytes = static_cast<std::size_t>((n + 7) / 8);
    auto buf = new_buffer(bytes == 0 ? 1 : bytes);
    std::memset(writable(buf), 0, bytes);
    return buf;
}
void attach_bitmap(dftu_series* out, BufferView vbuf) {
    // Trailing bits past `length` are always 0 (new_bitmap zeroes the buffer
    // and only rows < length are set), so a whole-byte count is exact.
    const std::size_t nbytes = static_cast<std::size_t>((out->length + 7) / 8);
    const auto valid = static_cast<std::int64_t>(
        dstd::bits::popcount_bytes(vbuf.data(), nbytes));
    out->validity = std::move(vbuf);
    out->null_count = out->length - valid;
}

// Allocate a Float64 output of length a.length (no validity) and widen a's
// values into it.
dftu_series* widen_to_f64(const dftu_series* a) {
    auto* out = new dftu_series();
    out->type = TypeId::Float64;
    out->encoding = Encoding::Flat;
    out->length = a->length;
    out->null_count = a->null_count;
    out->validity = a->validity;
    out->data = new_buffer(buffer_bytes(TypeId::Float64, a->length));
    auto* o = reinterpret_cast<double*>(writable(out->data));
    DFTRACER_UTILS_NUMERIC_DISPATCH(a->type, widen_f64, *a, o)
    return out;
}

}  // namespace

Series abs(const Series& v) { return Series{dftu_series_abs(v.handle())}; }
Series clip(const Series& v, dftu_scalar lo, dftu_scalar hi) {
    return Series{dftu_series_clip(v.handle(), lo, hi)};
}
Series round(const Series& v) { return Series{dftu_series_round(v.handle())}; }
Series fillna(const Series& v, dftu_scalar fill) {
    return Series{dftu_series_fillna(v.handle(), fill)};
}
Series cumsum(const Series& v) {
    return Series{dftu_series_cumsum(v.handle())};
}
Series cummax(const Series& v) {
    return Series{dftu_series_cummax(v.handle())};
}
Series cummin(const Series& v) {
    return Series{dftu_series_cummin(v.handle())};
}
Series cum_prod(const Series& v) {
    return Series{dftu_series_cum_prod(v.handle())};
}
Series cum_count(const Series& v) {
    return Series{dftu_series_cum_count(v.handle())};
}
Series ceil(const Series& v) { return Series{dftu_series_ceil(v.handle())}; }
Series floor(const Series& v) { return Series{dftu_series_floor(v.handle())}; }
Series trunc(const Series& v) { return Series{dftu_series_trunc(v.handle())}; }
Series sign(const Series& v) { return Series{dftu_series_sign(v.handle())}; }
Series negate(const Series& v) {
    return Series{dftu_series_negate(v.handle())};
}
Series diff(const Series& v) { return Series{dftu_series_diff(v.handle())}; }
Series pct_change(const Series& v) {
    return Series{dftu_series_pct_change(v.handle())};
}
Series sqrt(const Series& v) { return Series{dftu_series_sqrt(v.handle())}; }
Series exp(const Series& v) { return Series{dftu_series_exp(v.handle())}; }
Series log(const Series& v) { return Series{dftu_series_log(v.handle())}; }
Series log1p(const Series& v) { return Series{dftu_series_log1p(v.handle())}; }
Series expm1(const Series& v) { return Series{dftu_series_expm1(v.handle())}; }

}  // namespace dftracer::utils::dataframe

using dftracer::utils::dataframe::TypeId;

dftu_series* dftu_series_abs(const dftu_series* a) {
    // A bool is its own absolute value, as pandas gives, in any encoding.
    if (a && a->type == TypeId::Bool) return dftu_series_share(a);
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_abs);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_abs(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type)) return nullptr;
    dftu_series* out = alloc_like(a, true);
    HWY_DYNAMIC_DISPATCH(AbsKernel)
    (static_cast<std::int32_t>(a->type), a->data.data(), writable(out->data),
     static_cast<std::size_t>(a->length));
    return out;
}

dftu_series* dftu_series_clip(const dftu_series* a, dftu_scalar lo,
                              dftu_scalar hi) {
    using namespace dftracer::utils::dataframe;
    if (!a) return nullptr;
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_clip, lo, hi);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_clip(flat_a, lo, hi));
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    dftu_series* out = alloc_like(a, true);
    HWY_DYNAMIC_DISPATCH(ClipKernel)
    (static_cast<std::int32_t>(a->type), a->data.data(), lo, hi,
     writable(out->data), static_cast<std::size_t>(a->length));
    return out;
}

dftu_series* dftu_series_round(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_round);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_round(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type)) return nullptr;
    dftu_series* out = alloc_like(a, true);
    HWY_DYNAMIC_DISPATCH(RoundKernel)
    (static_cast<std::int32_t>(a->type), a->data.data(), writable(out->data),
     static_cast<std::size_t>(a->length));
    return out;
}

dftu_series* dftu_series_ceil(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_ceil);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_ceil(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type)) return nullptr;
    dftu_series* out = alloc_like(a, true);
    HWY_DYNAMIC_DISPATCH(CeilKernel)
    (static_cast<std::int32_t>(a->type), a->data.data(), writable(out->data),
     static_cast<std::size_t>(a->length));
    return out;
}

dftu_series* dftu_series_floor(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_floor);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_floor(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type)) return nullptr;
    dftu_series* out = alloc_like(a, true);
    HWY_DYNAMIC_DISPATCH(FloorKernel)
    (static_cast<std::int32_t>(a->type), a->data.data(), writable(out->data),
     static_cast<std::size_t>(a->length));
    return out;
}

dftu_series* dftu_series_trunc(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_trunc);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_trunc(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type)) return nullptr;
    dftu_series* out = alloc_like(a, true);
    HWY_DYNAMIC_DISPATCH(TruncKernel)
    (static_cast<std::int32_t>(a->type), a->data.data(), writable(out->data),
     static_cast<std::size_t>(a->length));
    return out;
}

dftu_series* dftu_series_sign(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_sign);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_sign(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    dftu_series* out = alloc_like(a, true);
    HWY_DYNAMIC_DISPATCH(SignKernel)
    (static_cast<std::int32_t>(a->type), a->data.data(), writable(out->data),
     static_cast<std::size_t>(a->length));
    return out;
}

dftu_series* dftu_series_negate(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_negate);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_negate(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    dftu_series* out = alloc_like(a, true);
    HWY_DYNAMIC_DISPATCH(NegateKernel)
    (static_cast<std::int32_t>(a->type), a->data.data(), writable(out->data),
     static_cast<std::size_t>(a->length));
    return out;
}

dftu_series* dftu_series_fillna(const dftu_series* a, dftu_scalar fill) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_fillna(flat_a, fill));

    using namespace dftracer::utils::dataframe;
    if (fill.kind == DFTU_SCALAR_TAG_NULL && a->encoding == Encoding::Flat &&
        (a->type == TypeId::Bool || a->type == TypeId::String ||
         is_numeric(a->type)))
        return dftu_series_share(a);
    if (a->encoding == Encoding::Flat && a->type == TypeId::Bool)
        return fillna_bool(a, fill);
    if (a->encoding == Encoding::Flat && a->type == TypeId::String)
        return fillna_string(a, fill);
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    if (fill.kind == DFTU_SCALAR_TAG_STR || fill.kind == DFTU_SCALAR_TAG_ERR)
        return nullptr;
    dftu_series* out = alloc_like(a, false);
    HWY_DYNAMIC_DISPATCH(FillKernel)
    (static_cast<std::int32_t>(a->type), a->data.data(),
     a->validity.block() ? a->validity.data() : nullptr, fill,
     writable(out->data), static_cast<std::size_t>(a->length));
    return out;
}

// The running scans skip a null and keep the row null (pandas / polars): the
// input's validity is the output's.
dftu_series* dftu_series_cumsum(const dftu_series* a) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_cumsum(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    dftu_series* out = alloc_like(a, true);
    if (!a->validity.block()) {
        HWY_DYNAMIC_DISPATCH(CumSumKernel)
        (static_cast<std::int32_t>(a->type), a->data.data(),
         writable(out->data), static_cast<std::size_t>(a->length));
    } else {
        DFTRACER_UTILS_NUMERIC_DISPATCH(a->type, cumsum_one, *a,
                                        writable(out->data))
    }
    return out;
}

dftu_series* dftu_series_cummax(const dftu_series* a) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_cummax(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    dftu_series* out = alloc_like(a, true);
    if (!a->validity.block()) {
        HWY_DYNAMIC_DISPATCH(CumMaxKernel)
        (static_cast<std::int32_t>(a->type), a->data.data(),
         writable(out->data), static_cast<std::size_t>(a->length));
    } else {
        DFTRACER_UTILS_NUMERIC_DISPATCH(a->type, cummax_one, *a,
                                        writable(out->data))
    }
    return out;
}

dftu_series* dftu_series_cummin(const dftu_series* a) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_cummin(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    dftu_series* out = alloc_like(a, true);
    if (!a->validity.block()) {
        HWY_DYNAMIC_DISPATCH(CumMinKernel)
        (static_cast<std::int32_t>(a->type), a->data.data(),
         writable(out->data), static_cast<std::size_t>(a->length));
    } else {
        DFTRACER_UTILS_NUMERIC_DISPATCH(a->type, cummin_one, *a,
                                        writable(out->data))
    }
    return out;
}

dftu_series* dftu_series_cum_prod(const dftu_series* a) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_cum_prod(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    dftu_series* out = alloc_like(a, true);
    if (!a->validity.block()) {
        HWY_DYNAMIC_DISPATCH(CumProdKernel)
        (static_cast<std::int32_t>(a->type), a->data.data(),
         writable(out->data), static_cast<std::size_t>(a->length));
    } else {
        DFTRACER_UTILS_NUMERIC_DISPATCH(a->type, cumprod_one, *a,
                                        writable(out->data))
    }
    return out;
}

constexpr std::int64_t CUM_ONE_SCAN_GRAIN = 1 << 20;

dftu_series* dftu_series_cum_count(const dftu_series* a) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_cum_count(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat) return nullptr;
    auto* out = new dftu_series();
    out->type = TypeId::Int64;
    out->encoding = Encoding::Flat;
    out->length = a->length;
    out->null_count = 0;
    out->data = new_buffer(buffer_bytes(TypeId::Int64, a->length));
    auto* o = reinterpret_cast<std::int64_t*>(writable(out->data));
    parallel_prefix_scan<std::int64_t>(
        a->length, CUM_ONE_SCAN_GRAIN, 0,
        [&](std::int64_t b, std::int64_t e) -> std::int64_t {
            std::int64_t acc = 0;
            for (std::int64_t i = b; i < e; ++i) {
                if (is_valid(*a, i)) ++acc;
                o[i] = acc;
            }
            return acc;
        },
        [&](std::int64_t b, std::int64_t e, std::int64_t off) {
            for (std::int64_t i = b; i < e; ++i) o[i] += off;
        },
        [](std::int64_t x, std::int64_t y) { return x + y; });
    return out;
}

dftu_series* dftu_series_diff(const dftu_series* a) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_diff(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    dftu_series* out = alloc_like(a, false);
    auto vbuf = new_bitmap(a->length);
    if (!a->validity.block()) {
        // Dense column: fill validity in bulk and let the branch-free data-only
        // diff auto-vectorize. null_count is exactly 1 (row 0).
        fill_valid_except_first(writable(vbuf), a->length);
        DFTRACER_UTILS_NUMERIC_DISPATCH(a->type, diff_data_one, *a,
                                        writable(out->data))
        out->validity = std::move(vbuf);
        out->null_count = a->length > 0 ? 1 : 0;
    } else {
        DFTRACER_UTILS_NUMERIC_DISPATCH(a->type, diff_one, *a,
                                        writable(out->data), writable(vbuf))
        attach_bitmap(out, std::move(vbuf));
    }
    return out;
}

dftu_series* dftu_series_pct_change(const dftu_series* a) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_pct_change(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    auto* out = new dftu_series();
    out->type = TypeId::Float64;
    out->encoding = Encoding::Flat;
    out->length = a->length;
    out->null_count = 0;
    out->data = new_buffer(buffer_bytes(TypeId::Float64, a->length));
    auto vbuf = new_bitmap(a->length);
    auto* o = reinterpret_cast<double*>(writable(out->data));
    if (!a->validity.block()) {
        // Dense column: bulk validity + branch-free data-only pct_change (any
        // numeric type widens to double). null_count is exactly 1 (row 0).
        fill_valid_except_first(writable(vbuf), a->length);
        DFTRACER_UTILS_NUMERIC_DISPATCH(a->type, pctchange_data_one, *a, o)
        out->validity = std::move(vbuf);
        out->null_count = a->length > 0 ? 1 : 0;
    } else {
        DFTRACER_UTILS_NUMERIC_DISPATCH(a->type, pctchange_one, *a, o,
                                        writable(vbuf))
        attach_bitmap(out, std::move(vbuf));
    }
    return out;
}

dftu_series* dftu_series_sqrt(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_sqrt);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_sqrt(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    dftu_series* out = widen_to_f64(a);
    HWY_DYNAMIC_DISPATCH(SqrtKernel)
    (reinterpret_cast<double*>(writable(out->data)),
     static_cast<std::size_t>(a->length));
    return out;
}

dftu_series* dftu_series_exp(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_exp);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_exp(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    dftu_series* out = widen_to_f64(a);
    HWY_DYNAMIC_DISPATCH(ExpKernel)
    (reinterpret_cast<double*>(writable(out->data)),
     static_cast<std::size_t>(a->length));
    return out;
}

dftu_series* dftu_series_log(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_log);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_log(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    dftu_series* out = widen_to_f64(a);
    HWY_DYNAMIC_DISPATCH(LogKernel)
    (reinterpret_cast<double*>(writable(out->data)),
     static_cast<std::size_t>(a->length));
    return out;
}

// log1p and expm1 stay precise for a tiny x, where log(1 + x) and exp(x) - 1
// lose every digit; libm gives them, one value at a time.
dftu_series* dftu_series_log1p(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_log1p);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_log1p(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    dftu_series* out = widen_to_f64(a);
    double* p = reinterpret_cast<double*>(writable(out->data));
    for (std::int64_t i = 0; i < a->length; ++i) p[i] = std::log1p(p[i]);
    return out;
}

dftu_series* dftu_series_expm1(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_expm1);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_expm1(flat_a));

    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric(a->type) ||
        a->type == TypeId::Bool)
        return nullptr;
    dftu_series* out = widen_to_f64(a);
    double* p = reinterpret_cast<double*>(writable(out->data));
    for (std::int64_t i = 0; i < a->length; ++i) p[i] = std::expm1(p[i]);
    return out;
}

namespace {

// A float column of the same length and validity as `a`: Float32 for a Float32
// input, else Float64 (the values widened).
template <class F>
dftu_series* math_unary(const dftu_series* a, F f) {
    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_numeric_dispatchable(a->type))
        return nullptr;
    if (a->type == TypeId::Float32) {
        auto* out = new dftu_series();
        out->type = TypeId::Float32;
        out->encoding = Encoding::Flat;
        out->length = a->length;
        out->null_count = a->null_count;
        out->validity = a->validity;
        out->data = new_buffer(buffer_bytes(TypeId::Float32, a->length));
        const auto* in = reinterpret_cast<const float*>(a->data.data());
        auto* o = reinterpret_cast<float*>(writable(out->data));
        for (std::int64_t i = 0; i < a->length; ++i) o[i] = f(in[i]);
        return out;
    }
    dftu_series* out = widen_to_f64(a);
    auto* p = reinterpret_cast<double*>(writable(out->data));
    for (std::int64_t i = 0; i < a->length; ++i) p[i] = f(p[i]);
    return out;
}

}  // namespace

dftu_series* dftu_series_sin(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_sin);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_sin(flat_a));
    return math_unary(a, [](auto x) { return std::sin(x); });
}

dftu_series* dftu_series_cos(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_cos);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_cos(flat_a));
    return math_unary(a, [](auto x) { return std::cos(x); });
}

dftu_series* dftu_series_tan(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_tan);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_tan(flat_a));
    return math_unary(a, [](auto x) { return std::tan(x); });
}

dftu_series* dftu_series_cot(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_cot);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_cot(flat_a));
    return math_unary(
        a, [](auto x) { return static_cast<decltype(x)>(1) / std::tan(x); });
}

dftu_series* dftu_series_arcsin(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_arcsin);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_arcsin(flat_a));
    return math_unary(a, [](auto x) { return std::asin(x); });
}

dftu_series* dftu_series_arccos(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_arccos);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_arccos(flat_a));
    return math_unary(a, [](auto x) { return std::acos(x); });
}

dftu_series* dftu_series_arctan(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_arctan);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_arctan(flat_a));
    return math_unary(a, [](auto x) { return std::atan(x); });
}

dftu_series* dftu_series_sinh(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_sinh);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_sinh(flat_a));
    return math_unary(a, [](auto x) { return std::sinh(x); });
}

dftu_series* dftu_series_cosh(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_cosh);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_cosh(flat_a));
    return math_unary(a, [](auto x) { return std::cosh(x); });
}

dftu_series* dftu_series_tanh(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_tanh);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_tanh(flat_a));
    return math_unary(a, [](auto x) { return std::tanh(x); });
}

dftu_series* dftu_series_arcsinh(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_arcsinh);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_arcsinh(flat_a));
    return math_unary(a, [](auto x) { return std::asinh(x); });
}

dftu_series* dftu_series_arccosh(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_arccosh);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_arccosh(flat_a));
    return math_unary(a, [](auto x) { return std::acosh(x); });
}

dftu_series* dftu_series_arctanh(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_arctanh);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_arctanh(flat_a));
    return math_unary(a, [](auto x) { return std::atanh(x); });
}

dftu_series* dftu_series_cbrt(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_cbrt);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_cbrt(flat_a));
    return math_unary(a, [](auto x) { return std::cbrt(x); });
}

dftu_series* dftu_series_log10(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_log10);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_log10(flat_a));
    return math_unary(a, [](auto x) { return std::log10(x); });
}

dftu_series* dftu_series_erf(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_erf);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_erf(flat_a));
    return math_unary(a, [](auto x) { return std::erf(x); });
}

dftu_series* dftu_series_erfc(const dftu_series* a) {
    DFTU_DICTIONARY_ELEMENTWISE(a, dftu_series_erfc);
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_erfc(flat_a));
    return math_unary(a, [](auto x) { return std::erfc(x); });
}

namespace {

enum class BitCount {
    Ones,
    Zeros,
    LeadingOnes,
    LeadingZeros,
    TrailingOnes,
    TrailingZeros
};

// One run over an integer column of type T; the unsigned twin of T gives the
// width and the two's complement bits of a signed value.
template <class T>
void bit_count_run(BitCount k, const T* in, std::uint32_t* out,
                   std::int64_t n) {
    using U = std::make_unsigned_t<T>;
    constexpr auto WIDTH = static_cast<std::uint32_t>(sizeof(U) * 8);
    auto run = [&](auto f) {
        for (std::int64_t i = 0; i < n; ++i)
            out[i] = static_cast<std::uint32_t>(f(static_cast<U>(in[i])));
    };
    switch (k) {
        case BitCount::Ones:
            run([](U u) { return std::popcount(u); });
            break;
        case BitCount::Zeros:
            run([](U u) { return WIDTH - std::popcount(u); });
            break;
        case BitCount::LeadingOnes:
            run([](U u) { return std::countl_one(u); });
            break;
        case BitCount::LeadingZeros:
            run([](U u) { return std::countl_zero(u); });
            break;
        case BitCount::TrailingOnes:
            run([](U u) { return std::countr_one(u); });
            break;
        case BitCount::TrailingZeros:
            run([](U u) { return std::countr_zero(u); });
            break;
    }
}

// A Bool is one bit: a count is the bit or its complement, and a run of one
// bit is the same count.
void bit_count_bool(BitCount k, const std::uint8_t* bits, std::uint32_t* out,
                    std::int64_t n) {
    const bool want_set = k == BitCount::Ones || k == BitCount::LeadingOnes ||
                          k == BitCount::TrailingOnes;
    for (std::int64_t i = 0; i < n; ++i) {
        const bool bit = ((bits[i >> 3] >> (i & 7)) & 1) != 0;
        out[i] = bit == want_set ? 1 : 0;
    }
}

bool is_bit_count_type(dftracer::utils::dataframe::TypeId t) {
    using dftracer::utils::dataframe::TypeId;
    return t == TypeId::Bool ||
           (dftracer::utils::dataframe::is_numeric_dispatchable(t) &&
            t != TypeId::Float32 && t != TypeId::Float64);
}

dftu_series* bit_count_series(const dftu_series* a, BitCount k) {
    using namespace dftracer::utils::dataframe;
    if (a->encoding != Encoding::Flat || !is_bit_count_type(a->type))
        return nullptr;
    auto* out = new dftu_series();
    out->type = TypeId::Uint32;
    out->encoding = Encoding::Flat;
    out->length = a->length;
    out->null_count = a->null_count;
    out->validity = a->validity;
    out->data = new_buffer(buffer_bytes(TypeId::Uint32, a->length));
    auto* o = reinterpret_cast<std::uint32_t*>(writable(out->data));
    const void* in = a->data.data();
    switch (a->type) {
        case TypeId::Bool:
            bit_count_bool(k, static_cast<const std::uint8_t*>(in), o,
                           a->length);
            break;
        case TypeId::Int8:
            bit_count_run(k, static_cast<const std::int8_t*>(in), o, a->length);
            break;
        case TypeId::Int16:
            bit_count_run(k, static_cast<const std::int16_t*>(in), o,
                          a->length);
            break;
        case TypeId::Int32:
            bit_count_run(k, static_cast<const std::int32_t*>(in), o,
                          a->length);
            break;
        case TypeId::Int64:
            bit_count_run(k, static_cast<const std::int64_t*>(in), o,
                          a->length);
            break;
        case TypeId::Uint8:
            bit_count_run(k, static_cast<const std::uint8_t*>(in), o,
                          a->length);
            break;
        case TypeId::Uint16:
            bit_count_run(k, static_cast<const std::uint16_t*>(in), o,
                          a->length);
            break;
        case TypeId::Uint32:
            bit_count_run(k, static_cast<const std::uint32_t*>(in), o,
                          a->length);
            break;
        case TypeId::Uint64:
            bit_count_run(k, static_cast<const std::uint64_t*>(in), o,
                          a->length);
            break;
        default:
            break;
    }
    return out;
}

}  // namespace

dftu_series* dftu_series_bitwise_count_ones(const dftu_series* a) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_bitwise_count_ones(flat_a));
    return bit_count_series(a, BitCount::Ones);
}

dftu_series* dftu_series_bitwise_count_zeros(const dftu_series* a) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_bitwise_count_zeros(flat_a));
    return bit_count_series(a, BitCount::Zeros);
}

dftu_series* dftu_series_bitwise_leading_ones(const dftu_series* a) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_bitwise_leading_ones(flat_a));
    return bit_count_series(a, BitCount::LeadingOnes);
}

dftu_series* dftu_series_bitwise_leading_zeros(const dftu_series* a) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_bitwise_leading_zeros(flat_a));
    return bit_count_series(a, BitCount::LeadingZeros);
}

dftu_series* dftu_series_bitwise_trailing_ones(const dftu_series* a) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_bitwise_trailing_ones(flat_a));
    return bit_count_series(a, BitCount::TrailingOnes);
}

dftu_series* dftu_series_bitwise_trailing_zeros(const dftu_series* a) {
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_bitwise_trailing_zeros(flat_a));
    return bit_count_series(a, BitCount::TrailingZeros);
}

#endif  // HWY_ONCE
