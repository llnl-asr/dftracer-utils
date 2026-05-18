#include <dftracer/utils/utilities/common/statistics/distributions.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <stdexcept>

// Boost.Math standalone is configured globally via -DBOOST_MATH_STANDALONE.
#include <boost/math/distributions/exponential.hpp>
#include <boost/math/distributions/gamma.hpp>
#include <boost/math/distributions/lognormal.hpp>
#include <boost/math/distributions/normal.hpp>
#include <boost/math/distributions/weibull.hpp>

namespace dftracer::utils::utilities::common::statistics {

namespace bm = boost::math;

namespace {

constexpr double kMinPositive = 1e-12;
constexpr int kNewtonMaxIter = 100;
constexpr double kNewtonTol = 1e-8;

// Sample statistics computed in one pass via Welford for numerical stability.
struct SampleSummary {
    std::size_t n = 0;
    double mean = 0.0;
    double variance = 0.0;  // population variance (1/n)
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    bool any_non_positive = false;
};

SampleSummary summarize(const std::vector<double>& data) {
    SampleSummary s;
    double m2 = 0.0;
    for (double x : data) {
        if (x <= 0.0) s.any_non_positive = true;
        ++s.n;
        const double delta = x - s.mean;
        s.mean += delta / static_cast<double>(s.n);
        m2 += delta * (x - s.mean);
        if (x < s.min) s.min = x;
        if (x > s.max) s.max = x;
    }
    if (s.n > 0) s.variance = m2 / static_cast<double>(s.n);
    return s;
}

template <typename DistCdf>
double ks_statistic(const std::vector<double>& sorted_data,
                    DistCdf&& dist_cdf) {
    const auto n = static_cast<double>(sorted_data.size());
    double max_diff = 0.0;
    // Two-sided KS: at each xi the empirical CDF jumps from (i-1)/n to i/n.
    // Compare the theoretical CDF against both sides.
    for (std::size_t i = 0; i < sorted_data.size(); ++i) {
        const double x = sorted_data[i];
        const double f_theo = dist_cdf(x);
        const double f_lo = static_cast<double>(i) / n;
        const double f_hi = static_cast<double>(i + 1) / n;
        max_diff = std::max(max_diff, std::abs(f_theo - f_lo));
        max_diff = std::max(max_diff, std::abs(f_hi - f_theo));
    }
    return max_diff;
}

template <typename DistPdf>
double log_likelihood(const std::vector<double>& data, DistPdf&& dist_pdf) {
    double ll = 0.0;
    for (double x : data) {
        const double p = dist_pdf(x);
        if (p <= 0.0 || !std::isfinite(p))
            return -std::numeric_limits<double>::infinity();
        ll += std::log(p);
    }
    return ll;
}

double compute_bic(double log_l, std::size_t n, int k) {
    return static_cast<double>(k) * std::log(static_cast<double>(n)) -
           2.0 * log_l;
}

// ---- Per-distribution MLE -------------------------------------------------

FittedDistribution fit_normal(const std::vector<double>& data,
                              const SampleSummary& s) {
    FittedDistribution f;
    f.kind = DistributionKind::Normal;
    if (s.n < 2 || s.variance <= 0.0) return f;
    const double sigma = std::sqrt(s.variance);
    f.params = {s.mean, sigma, 0.0};
    f.valid = true;

    bm::normal dist(s.mean, sigma);
    auto sorted = data;
    std::sort(sorted.begin(), sorted.end());
    f.ks_stat =
        ks_statistic(sorted, [&](double x) { return bm::cdf(dist, x); });
    f.log_likelihood =
        log_likelihood(data, [&](double x) { return bm::pdf(dist, x); });
    f.bic = compute_bic(f.log_likelihood, s.n, free_parameter_count(f.kind));
    return f;
}

FittedDistribution fit_lognormal(const std::vector<double>& data,
                                 const SampleSummary& s) {
    FittedDistribution f;
    f.kind = DistributionKind::Lognormal;
    if (s.n < 2 || s.any_non_positive) return f;

    double mean_log = 0.0;
    for (double x : data) mean_log += std::log(x);
    mean_log /= static_cast<double>(s.n);

    double var_log = 0.0;
    for (double x : data) {
        const double d = std::log(x) - mean_log;
        var_log += d * d;
    }
    var_log /= static_cast<double>(s.n);
    if (var_log <= 0.0) return f;

    const double sigma = std::sqrt(var_log);
    f.params = {mean_log, sigma, 0.0};
    f.valid = true;

    bm::lognormal dist(mean_log, sigma);
    auto sorted = data;
    std::sort(sorted.begin(), sorted.end());
    f.ks_stat =
        ks_statistic(sorted, [&](double x) { return bm::cdf(dist, x); });
    f.log_likelihood =
        log_likelihood(data, [&](double x) { return bm::pdf(dist, x); });
    f.bic = compute_bic(f.log_likelihood, s.n, free_parameter_count(f.kind));
    return f;
}

FittedDistribution fit_exponential(const std::vector<double>& data,
                                   const SampleSummary& s) {
    FittedDistribution f;
    f.kind = DistributionKind::Exponential;
    if (s.n < 1 || s.mean <= 0.0 || s.any_non_positive) return f;
    const double rate = 1.0 / s.mean;
    f.params = {rate, 0.0, 0.0};
    f.valid = true;

    bm::exponential dist(rate);
    auto sorted = data;
    std::sort(sorted.begin(), sorted.end());
    f.ks_stat =
        ks_statistic(sorted, [&](double x) { return bm::cdf(dist, x); });
    f.log_likelihood =
        log_likelihood(data, [&](double x) { return bm::pdf(dist, x); });
    f.bic = compute_bic(f.log_likelihood, s.n, free_parameter_count(f.kind));
    return f;
}

// Gamma MLE: there's no closed form. We use method-of-moments as the initial
// estimate (good enough for most timing distributions) and then refine the
// shape parameter via Newton-Raphson on the log-likelihood derivative.
//
//   d/dk log L = n*ln(k/mean) - n*digamma(k) + sum(ln x_i)
//
// digamma(k) is the polygamma_0; both digamma and its derivative (trigamma)
// are provided by Boost.Math.
FittedDistribution fit_gamma(const std::vector<double>& data,
                             const SampleSummary& s) {
    FittedDistribution f;
    f.kind = DistributionKind::Gamma;
    if (s.n < 2 || s.any_non_positive || s.variance <= 0.0) return f;

    // Method-of-moments initial estimate.
    double k = s.mean * s.mean / s.variance;
    if (k <= 0.0 || !std::isfinite(k)) return f;

    double sum_log = 0.0;
    for (double x : data) sum_log += std::log(x);
    const double mean_log = sum_log / static_cast<double>(s.n);
    const double log_mean = std::log(s.mean);
    // s = log_mean - mean_log; for k > 0, k satisfies
    //   ln(k) - digamma(k) = s.
    const double rhs = log_mean - mean_log;
    if (rhs <= 0.0) {
        // Data is degenerate; fall back to MoM.
    } else {
        for (int it = 0; it < kNewtonMaxIter; ++it) {
            const double g = std::log(k) - bm::digamma(k) - rhs;
            const double gp = 1.0 / k - bm::trigamma(k);
            if (!std::isfinite(g) || !std::isfinite(gp) || gp == 0.0) break;
            const double dk = g / gp;
            k -= dk;
            if (k <= kMinPositive) {
                k = kMinPositive;
                break;
            }
            if (std::abs(dk) < kNewtonTol) break;
        }
    }
    const double theta = s.mean / k;
    if (k <= 0.0 || theta <= 0.0) return f;
    f.params = {k, theta, 0.0};
    f.valid = true;

    bm::gamma_distribution<double> dist(k, theta);
    auto sorted = data;
    std::sort(sorted.begin(), sorted.end());
    f.ks_stat =
        ks_statistic(sorted, [&](double x) { return bm::cdf(dist, x); });
    f.log_likelihood =
        log_likelihood(data, [&](double x) { return bm::pdf(dist, x); });
    f.bic = compute_bic(f.log_likelihood, s.n, free_parameter_count(f.kind));
    return f;
}

// Weibull MLE: shape `k` is the root of
//   f(k) = sum(x^k ln x) / sum(x^k) - 1/k - mean(ln x) = 0
// Newton-Raphson with MoM-style initial estimate.
FittedDistribution fit_weibull(const std::vector<double>& data,
                               const SampleSummary& s) {
    FittedDistribution f;
    f.kind = DistributionKind::Weibull;
    if (s.n < 2 || s.any_non_positive || s.variance <= 0.0) return f;

    double sum_log = 0.0;
    for (double x : data) sum_log += std::log(x);
    const double mean_log = sum_log / static_cast<double>(s.n);

    // Initial shape via rough variance heuristic; ~1.0 works for most cases.
    double k = 1.0;

    for (int it = 0; it < kNewtonMaxIter; ++it) {
        double s_xk = 0.0, s_xk_lnx = 0.0, s_xk_lnx2 = 0.0;
        for (double x : data) {
            const double lx = std::log(x);
            const double xk = std::pow(x, k);
            s_xk += xk;
            s_xk_lnx += xk * lx;
            s_xk_lnx2 += xk * lx * lx;
        }
        if (s_xk <= 0.0 || !std::isfinite(s_xk)) return f;
        const double a = s_xk_lnx / s_xk;
        const double a_prime =
            (s_xk_lnx2 * s_xk - s_xk_lnx * s_xk_lnx) / (s_xk * s_xk);
        const double g = a - 1.0 / k - mean_log;
        const double gp = a_prime + 1.0 / (k * k);
        if (!std::isfinite(g) || !std::isfinite(gp) || gp == 0.0) break;
        const double dk = g / gp;
        k -= dk;
        if (k <= kMinPositive) {
            k = kMinPositive;
            break;
        }
        if (std::abs(dk) < kNewtonTol) break;
    }

    double s_xk = 0.0;
    for (double x : data) s_xk += std::pow(x, k);
    const double lambda = std::pow(s_xk / static_cast<double>(s.n), 1.0 / k);
    if (k <= 0.0 || lambda <= 0.0) return f;
    f.params = {k, lambda, 0.0};
    f.valid = true;

    bm::weibull dist(k, lambda);
    auto sorted = data;
    std::sort(sorted.begin(), sorted.end());
    f.ks_stat =
        ks_statistic(sorted, [&](double x) { return bm::cdf(dist, x); });
    f.log_likelihood =
        log_likelihood(data, [&](double x) { return bm::pdf(dist, x); });
    f.bic = compute_bic(f.log_likelihood, s.n, free_parameter_count(f.kind));
    return f;
}

}  // namespace

std::string_view distribution_name(DistributionKind k) {
    switch (k) {
        case DistributionKind::Normal:
            return "Normal";
        case DistributionKind::Lognormal:
            return "Lognormal";
        case DistributionKind::Gamma:
            return "Gamma";
        case DistributionKind::Exponential:
            return "Exponential";
        case DistributionKind::Weibull:
            return "Weibull";
    }
    return "Unknown";
}

int free_parameter_count(DistributionKind kind) {
    switch (kind) {
        case DistributionKind::Normal:
        case DistributionKind::Lognormal:
        case DistributionKind::Gamma:
        case DistributionKind::Weibull:
            return 2;
        case DistributionKind::Exponential:
            return 1;
    }
    return 0;
}

FittedDistribution fit_single_distribution(DistributionKind kind,
                                           const std::vector<double>& data) {
    const auto s = summarize(data);
    switch (kind) {
        case DistributionKind::Normal:
            return fit_normal(data, s);
        case DistributionKind::Lognormal:
            return fit_lognormal(data, s);
        case DistributionKind::Gamma:
            return fit_gamma(data, s);
        case DistributionKind::Exponential:
            return fit_exponential(data, s);
        case DistributionKind::Weibull:
            return fit_weibull(data, s);
    }
    return {};
}

std::vector<FittedDistribution> fit_all_single_distributions(
    const std::vector<double>& data) {
    const auto s = summarize(data);
    std::vector<FittedDistribution> fits;
    fits.reserve(5);
    fits.push_back(fit_normal(data, s));
    fits.push_back(fit_lognormal(data, s));
    fits.push_back(fit_gamma(data, s));
    fits.push_back(fit_exponential(data, s));
    fits.push_back(fit_weibull(data, s));

    std::sort(fits.begin(), fits.end(),
              [](const FittedDistribution& a, const FittedDistribution& b) {
                  if (a.valid != b.valid) return a.valid;  // valid first
                  return a.ks_stat < b.ks_stat;
              });
    return fits;
}

std::optional<FittedDistribution> best_fit_by_ks(
    const std::vector<FittedDistribution>& fits) {
    for (const auto& f : fits) {
        if (f.valid) return f;
    }
    return std::nullopt;
}

double pdf(const FittedDistribution& fit, double x) {
    switch (fit.kind) {
        case DistributionKind::Normal:
            return bm::pdf(bm::normal(fit.params[0], fit.params[1]), x);
        case DistributionKind::Lognormal:
            return bm::pdf(bm::lognormal(fit.params[0], fit.params[1]), x);
        case DistributionKind::Gamma:
            return bm::pdf(
                bm::gamma_distribution<double>(fit.params[0], fit.params[1]),
                x);
        case DistributionKind::Exponential:
            return bm::pdf(bm::exponential(fit.params[0]), x);
        case DistributionKind::Weibull:
            return bm::pdf(bm::weibull(fit.params[0], fit.params[1]), x);
    }
    return 0.0;
}

double cdf(const FittedDistribution& fit, double x) {
    switch (fit.kind) {
        case DistributionKind::Normal:
            return bm::cdf(bm::normal(fit.params[0], fit.params[1]), x);
        case DistributionKind::Lognormal:
            return bm::cdf(bm::lognormal(fit.params[0], fit.params[1]), x);
        case DistributionKind::Gamma:
            return bm::cdf(
                bm::gamma_distribution<double>(fit.params[0], fit.params[1]),
                x);
        case DistributionKind::Exponential:
            return bm::cdf(bm::exponential(fit.params[0]), x);
        case DistributionKind::Weibull:
            return bm::cdf(bm::weibull(fit.params[0], fit.params[1]), x);
    }
    return 0.0;
}

double quantile(const FittedDistribution& fit, double p) {
    switch (fit.kind) {
        case DistributionKind::Normal:
            return bm::quantile(bm::normal(fit.params[0], fit.params[1]), p);
        case DistributionKind::Lognormal:
            return bm::quantile(bm::lognormal(fit.params[0], fit.params[1]), p);
        case DistributionKind::Gamma:
            return bm::quantile(
                bm::gamma_distribution<double>(fit.params[0], fit.params[1]),
                p);
        case DistributionKind::Exponential:
            return bm::quantile(bm::exponential(fit.params[0]), p);
        case DistributionKind::Weibull:
            return bm::quantile(bm::weibull(fit.params[0], fit.params[1]), p);
    }
    return 0.0;
}

Sampler make_sampler(const FittedDistribution& fit,
                     std::optional<double> min_bound,
                     std::optional<double> max_bound) {
    if (!fit.valid) {
        throw std::invalid_argument(
            "make_sampler called with invalid FittedDistribution");
    }
    const auto p0 = fit.params[0];
    const auto p1 = fit.params[1];
    const auto kind = fit.kind;

    auto draw = [kind, p0, p1](std::mt19937_64& rng) -> double {
        switch (kind) {
            case DistributionKind::Normal:
                return std::normal_distribution<double>(p0, p1)(rng);
            case DistributionKind::Lognormal:
                return std::lognormal_distribution<double>(p0, p1)(rng);
            case DistributionKind::Gamma:
                return std::gamma_distribution<double>(p0, p1)(rng);
            case DistributionKind::Exponential:
                return std::exponential_distribution<double>(p0)(rng);
            case DistributionKind::Weibull:
                return std::weibull_distribution<double>(p0, p1)(rng);
        }
        return 0.0;
    };

    if (!min_bound && !max_bound) {
        return [draw](std::mt19937_64& rng) { return draw(rng); };
    }
    const double lo =
        min_bound.value_or(-std::numeric_limits<double>::infinity());
    const double hi =
        max_bound.value_or(std::numeric_limits<double>::infinity());
    return [draw, lo, hi](std::mt19937_64& rng) {
        return std::clamp(draw(rng), lo, hi);
    };
}

}  // namespace dftracer::utils::utilities::common::statistics
