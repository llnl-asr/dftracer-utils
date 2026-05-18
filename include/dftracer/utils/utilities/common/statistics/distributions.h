#ifndef DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_DISTRIBUTIONS_H
#define DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_DISTRIBUTIONS_H

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::common::statistics {

enum class DistributionKind : std::uint8_t {
    Normal,       // params = {mean, stddev, _}
    Lognormal,    // params = {mu, sigma, _}      (mu, sigma in log space)
    Gamma,        // params = {shape, scale, _}
    Exponential,  // params = {rate, _, _}        (rate = 1/scale)
    Weibull,      // params = {shape, scale, _}
};

std::string_view distribution_name(DistributionKind k);

// Result of fitting a single distribution to a sample array.
// `params` semantics depend on `kind`.
struct FittedDistribution {
    DistributionKind kind;
    std::array<double, 3> params{};
    double ks_stat = 1.0;  // Kolmogorov-Smirnov statistic (lower = better)
    double log_likelihood = 0.0;  // sum of log pdf(x_i)
    double bic = 0.0;             // k*ln(n) - 2*log_likelihood
    bool valid = false;           // true when MLE succeeded
};

// MLE fit for a single distribution. Returns valid=false when fitting fails
// (e.g. non-positive data for lognormal, sample size < 2, Newton
// non-convergence).
FittedDistribution fit_single_distribution(DistributionKind kind,
                                           const std::vector<double>& data);

// Fits all five distributions and returns them ordered by ascending ks_stat.
// Invalid fits are kept at the back of the result.
std::vector<FittedDistribution> fit_all_single_distributions(
    const std::vector<double>& data);

// Picks the lowest-KS valid fit. Returns nullopt if none of the fits succeeded.
std::optional<FittedDistribution> best_fit_by_ks(
    const std::vector<FittedDistribution>& fits);

// Distribution PDF / CDF / inverse-CDF. Behavior is undefined when
// `fit.valid == false`.
double pdf(const FittedDistribution& fit, double x);
double cdf(const FittedDistribution& fit, double x);
double quantile(const FittedDistribution& fit, double p);

// Sampler signature. Matches dlio::Sampler so dlio::BarrierSimulator can
// consume it directly without an explicit cast.
using Sampler = std::function<double(std::mt19937_64&)>;

// Builds a Sampler from a fitted distribution.
// Optional min/max bounds clamp the output (applied after sampling).
Sampler make_sampler(const FittedDistribution& fit,
                     std::optional<double> min_bound = std::nullopt,
                     std::optional<double> max_bound = std::nullopt);

// Returns the parameter count used for BIC. Useful when extending to mixtures.
int free_parameter_count(DistributionKind kind);

}  // namespace dftracer::utils::utilities::common::statistics

#endif
