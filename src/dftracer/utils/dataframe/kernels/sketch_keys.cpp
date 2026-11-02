// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/sketch.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/sketch_keys.cpp"
#include <hwy/foreach_target.h>  // must precede highway.h
#include <hwy/highway.h>
// math-inl.h is an -inl.h: it participates in foreach_target and must follow
// highway.h. Provides the vectorized transcendental hn::Log.
#include <hwy/contrib/math/math-inl.h>

HWY_BEFORE_NAMESPACE();
namespace dftracer::utils::dataframe {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// key = ceil(log(|x|) / log_gamma); an exact zero maps to SKETCH_ZERO_KEY. The
// logarithm is the sketch's one costly per-value step, hoisted out of the
// scalar bin scatter and vectorized here.
void BucketKeysImpl(const double* values, std::size_t n, double inv_log_gamma,
                    std::int32_t* out) {
    const hn::ScalableTag<double> d;
    const hn::Rebind<std::int32_t, decltype(d)> di32;
    const std::size_t lanes = hn::Lanes(d);
    const auto inv = hn::Set(d, inv_log_gamma);
    const auto zero = hn::Zero(d);
    // Substitute the zero sentinel in the double domain (the mask matches the
    // double lanes, so no cross-width RebindMask), then demote to int32. The
    // sentinel is exactly representable as a double and demotes back to itself.
    const auto zkey = hn::Set(d, static_cast<double>(SKETCH_ZERO_KEY));

    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes) {
        const auto v = hn::LoadU(d, values + i);
        const auto k = hn::Ceil(hn::Mul(hn::Log(d, hn::Abs(v)), inv));
        const auto kz = hn::IfThenElse(hn::Eq(v, zero), zkey, k);
        hn::StoreU(hn::DemoteTo(di32, kz), di32, out + i);
    }
    for (; i < n; ++i) {
        const double x = values[i];
        out[i] = x == 0.0 ? SKETCH_ZERO_KEY
                          : static_cast<std::int32_t>(std::ceil(
                                std::log(std::abs(x)) * inv_log_gamma));
    }
}

}  // namespace HWY_NAMESPACE
}  // namespace dftracer::utils::dataframe
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace dftracer::utils::dataframe {

HWY_EXPORT(BucketKeysImpl);

void sketch_bucket_keys(const double* values, std::int64_t n, double log_gamma,
                        std::int32_t* out_keys) {
    if (n <= 0) return;
    HWY_DYNAMIC_DISPATCH(BucketKeysImpl)
    (values, static_cast<std::size_t>(n), 1.0 / log_gamma, out_keys);
}

}  // namespace dftracer::utils::dataframe
#endif
