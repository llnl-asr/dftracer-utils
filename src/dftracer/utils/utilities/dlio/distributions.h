// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_DLIO_DISTRIBUTIONS_H
#define DFTRACER_UTILS_UTILITIES_DLIO_DISTRIBUTIONS_H

#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>

#include <array>
#include <cstdint>
#include <random>

namespace dftracer::utils::utilities::dlio {

enum class DistributionKind : std::uint8_t {
    Normal,       ///< params = {mean, stddev, _}
    Lognormal,    ///< params = {mu, sigma, _}      (mu, sigma in log space)
    Gamma,        ///< params = {shape, scale, _}
    Exponential,  ///< params = {rate, _, _}        (rate = 1/scale)
    Weibull,      ///< params = {shape, scale, _}
};

/// Result of fitting a single distribution to a sample array.
/// `params` semantics depend on `kind`.
struct FittedDistribution {
    DistributionKind kind;
    std::array<double, 3> params{};
    double ks_stat = 1.0;  ///< Kolmogorov-Smirnov statistic (lower = better)
    double log_likelihood = 0.0;  ///< sum of log pdf(x_i)
    double bic = 0.0;             ///< k*ln(n) - 2*log_likelihood
    bool valid = false;           ///< true when MLE succeeded
};

/// MLE fit for a single distribution. Returns valid=false when fitting fails
/// (e.g. non-positive data for lognormal, sample size < 2, Newton
/// non-convergence).
FittedDistribution fit_single_distribution(DistributionKind kind,
                                           const dstd::Vector<double>& data);

/// Fits all five distributions and returns them ordered by ascending ks_stat.
/// Invalid fits are kept at the back of the result.
dstd::Vector<FittedDistribution> fit_all_single_distributions(
    const dstd::Vector<double>& data);

/// Picks the lowest-KS valid fit. Returns nullopt if none of the fits
/// succeeded.
dstd::Optional<FittedDistribution> best_fit_by_ks(
    const dstd::Vector<FittedDistribution>& fits);

/// Distribution PDF / CDF / inverse-CDF. Behavior is undefined when
/// `fit.valid == false`. An argument outside the domain (a NaN `x`, a `p`
/// outside [0, 1]) returns NaN, and an overflow returns infinity.
double pdf(const FittedDistribution& fit, double x);
double cdf(const FittedDistribution& fit, double x);
double quantile(const FittedDistribution& fit, double p);

/// Sampler signature. Matches dlio::Sampler so @ref dlio::BarrierSimulator can
/// consume it directly without an explicit cast.
using Sampler = dstd::Function<double(std::mt19937_64&)>;

/// Builds a Sampler from a fitted distribution. A fit with `valid == false` is
/// a broken contract and ends the process.
/// Optional min/max bounds clamp the output (applied after sampling).
Sampler make_sampler(const FittedDistribution& fit,
                     dstd::Optional<double> min_bound = dstd::nullopt,
                     dstd::Optional<double> max_bound = dstd::nullopt);

/// Returns the parameter count used for BIC. Useful when extending to mixtures.
int free_parameter_count(DistributionKind kind);

}  // namespace dftracer::utils::utilities::dlio

#endif
