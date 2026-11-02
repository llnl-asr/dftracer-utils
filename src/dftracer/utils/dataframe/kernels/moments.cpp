// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/internal/moments_simd.h>

#include <cstddef>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/moments.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::dataframe {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

double SumImpl(const double* x, std::size_t n) {
    const hn::ScalableTag<double> d;
    const std::size_t lanes = hn::Lanes(d);
    auto acc = hn::Zero(d);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes) acc = hn::Add(acc, hn::LoadU(d, x + i));
    double s = hn::ReduceSum(d, acc);
    for (; i < n; ++i) s += x[i];
    return s;
}

void CentralImpl(const double* x, std::size_t n, double mean, double* out) {
    const hn::ScalableTag<double> d;
    const std::size_t lanes = hn::Lanes(d);
    const auto vm = hn::Set(d, mean);
    auto a2 = hn::Zero(d), a3 = hn::Zero(d), a4 = hn::Zero(d);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes) {
        const auto dd = hn::Sub(hn::LoadU(d, x + i), vm);
        const auto d2 = hn::Mul(dd, dd);
        a2 = hn::Add(a2, d2);
        a3 = hn::Add(a3, hn::Mul(d2, dd));
        a4 = hn::Add(a4, hn::Mul(d2, d2));
    }
    double m2 = hn::ReduceSum(d, a2);
    double m3 = hn::ReduceSum(d, a3);
    double m4 = hn::ReduceSum(d, a4);
    for (; i < n; ++i) {
        const double dd = x[i] - mean;
        const double d2 = dd * dd;
        m2 += d2;
        m3 += d2 * dd;
        m4 += d2 * d2;
    }
    out[0] = m2;
    out[1] = m3;
    out[2] = m4;
}

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::dataframe
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::dataframe {

HWY_EXPORT(SumImpl);
HWY_EXPORT(CentralImpl);

double sum_f64(const double* x, std::size_t n) {
    if (n == 0) return 0.0;
    return HWY_DYNAMIC_DISPATCH(SumImpl)(x, n);
}

void central_moments(const double* x, std::size_t n, double mean,
                     double out[3]) {
    out[0] = out[1] = out[2] = 0.0;
    if (n == 0) return;
    HWY_DYNAMIC_DISPATCH(CentralImpl)(x, n, mean, out);
}

}  // namespace dftracer::utils::dataframe
#endif  // HWY_ONCE
