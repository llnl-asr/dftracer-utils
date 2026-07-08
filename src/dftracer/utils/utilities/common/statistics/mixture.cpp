#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/utilities/common/statistics/mixture.h>

#include <algorithm>
#include <boost/math/distributions/normal.hpp>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>

namespace dftracer::utils::utilities::common::statistics {

namespace bm = boost::math;

namespace {

constexpr double INV_SQRT_2PI = 0.3989422804014327;  // 1 / sqrt(2*pi)

inline double normal_pdf(double x, double mean, double stddev) {
    const double z = (x - mean) / stddev;
    return (INV_SQRT_2PI / stddev) * std::exp(-0.5 * z * z);
}

// log(sum_k exp(log_x[k])) computed with the standard log-sum-exp trick to
// avoid underflow when component log-likelihoods diverge.
double log_sum_exp(const std::vector<double>& log_x) {
    double m = -std::numeric_limits<double>::infinity();
    for (double v : log_x) {
        if (v > m) m = v;
    }
    if (!std::isfinite(m)) return m;
    double s = 0.0;
    for (double v : log_x) s += std::exp(v - m);
    return m + std::log(s);
}

// Pick K initial means by sampling sorted-data quantiles uniformly across (0,
// 1). Avoids the degenerate case where random init collapses all components
// together.
std::vector<double> initial_means(const std::vector<double>& sorted, int K) {
    std::vector<double> means;
    means.reserve(K);
    for (int k = 0; k < K; ++k) {
        const double q =
            (static_cast<double>(k) + 0.5) / static_cast<double>(K);
        const std::size_t idx = std::min<std::size_t>(
            sorted.size() - 1,
            static_cast<std::size_t>(q * static_cast<double>(sorted.size())));
        means.push_back(sorted[idx]);
    }
    return means;
}

double sample_variance(const std::vector<double>& data, double mean) {
    double sq = 0.0;
    for (double x : data) {
        const double d = x - mean;
        sq += d * d;
    }
    return sq / static_cast<double>(data.size());
}

}  // namespace

FittedMixture fit_gaussian_mixture(const std::vector<double>& data, int K,
                                   const MixtureFitOptions& options) {
    FittedMixture m;
    if (K <= 0 || data.size() < static_cast<std::size_t>(K) * 2) return m;

    auto sorted = data;
    std::sort(sorted.begin(), sorted.end());

    const double total_mean = std::accumulate(data.begin(), data.end(), 0.0) /
                              static_cast<double>(data.size());
    const double total_var =
        std::max(sample_variance(data, total_mean), options.variance_floor);

    m.weights.assign(K, 1.0 / static_cast<double>(K));
    m.components.resize(K);
    const auto means_init = initial_means(sorted, K);
    for (int k = 0; k < K; ++k) {
        m.components[k].mean = means_init[k];
        m.components[k].stddev = std::sqrt(total_var);
    }

    const std::size_t N = data.size();
    std::vector<std::vector<double>> resp(K, std::vector<double>(N, 0.0));
    double prev_ll = -std::numeric_limits<double>::infinity();

    std::vector<double> log_comp(K);
    for (int it = 0; it < options.max_iter; ++it) {
        // E-step: responsibilities via log-sum-exp.
        double ll = 0.0;
        for (std::size_t i = 0; i < N; ++i) {
            const double x = data[i];
            for (int k = 0; k < K; ++k) {
                const double p =
                    normal_pdf(x, m.components[k].mean, m.components[k].stddev);
                log_comp[k] = (p > 0.0 && std::isfinite(p))
                                  ? std::log(m.weights[k]) + std::log(p)
                                  : -std::numeric_limits<double>::infinity();
            }
            const double lse = log_sum_exp(log_comp);
            ll += lse;
            for (int k = 0; k < K; ++k) {
                resp[k][i] = std::exp(log_comp[k] - lse);
            }
        }

        // M-step.
        for (int k = 0; k < K; ++k) {
            double n_k = 0.0;
            for (std::size_t i = 0; i < N; ++i) n_k += resp[k][i];

            // Guard against an empty component.
            if (n_k < 1e-12) {
                m.weights[k] = 0.0;
                m.components[k].stddev =
                    std::sqrt(std::max(total_var, options.variance_floor));
                continue;
            }

            double mean = 0.0;
            for (std::size_t i = 0; i < N; ++i) mean += resp[k][i] * data[i];
            mean /= n_k;

            double var = 0.0;
            for (std::size_t i = 0; i < N; ++i) {
                const double d = data[i] - mean;
                var += resp[k][i] * d * d;
            }
            var /= n_k;
            if (var < options.variance_floor) var = options.variance_floor;

            m.weights[k] = n_k / static_cast<double>(N);
            m.components[k].mean = mean;
            m.components[k].stddev = std::sqrt(var);
        }

        m.iterations = it + 1;
        if (std::isfinite(ll) && std::abs(ll - prev_ll) < options.tol) {
            m.converged = true;
            prev_ll = ll;
            break;
        }
        prev_ll = ll;
    }

    // Final log-likelihood pass (uses the converged parameters).
    double ll = 0.0;
    for (double x : data) {
        for (int k = 0; k < K; ++k) {
            const double p =
                normal_pdf(x, m.components[k].mean, m.components[k].stddev);
            log_comp[k] = (p > 0.0 && std::isfinite(p))
                              ? std::log(m.weights[k]) + std::log(p)
                              : -std::numeric_limits<double>::infinity();
        }
        ll += log_sum_exp(log_comp);
    }
    m.log_likelihood = ll;
    m.bic = static_cast<double>(free_parameter_count(m)) *
                std::log(static_cast<double>(N)) -
            2.0 * ll;
    m.valid = std::isfinite(ll);
    return m;
}

int free_parameter_count(const FittedMixture& mix) {
    const int K = static_cast<int>(mix.weights.size());
    return 3 * K - 1;
}

double pdf(const FittedMixture& mix, double x) {
    double p = 0.0;
    for (std::size_t k = 0; k < mix.weights.size(); ++k) {
        p += mix.weights[k] *
             normal_pdf(x, mix.components[k].mean, mix.components[k].stddev);
    }
    return p;
}

double cdf(const FittedMixture& mix, double x) {
    double c = 0.0;
    for (std::size_t k = 0; k < mix.weights.size(); ++k) {
        c += mix.weights[k] * bm::cdf(bm::normal(mix.components[k].mean,
                                                 mix.components[k].stddev),
                                      x);
    }
    return c;
}

Sampler make_sampler(const FittedMixture& mix, std::optional<double> min_bound,
                     std::optional<double> max_bound) {
    if (!mix.valid) {
        throw DFTUtilsException(
            ErrorCode::INVALID_ARGUMENT,
            "make_sampler called with invalid FittedMixture");
    }

    auto weights = mix.weights;
    auto comps = mix.components;

    auto draw = [weights = std::move(weights),
                 comps = std::move(comps)](std::mt19937_64& rng) -> double {
        std::discrete_distribution<int> cat(weights.begin(), weights.end());
        const int k = cat(rng);
        return std::normal_distribution<double>(comps[k].mean,
                                                comps[k].stddev)(rng);
    };

    if (!min_bound && !max_bound) {
        return [draw = std::move(draw)](std::mt19937_64& rng) {
            return draw(rng);
        };
    }
    const double lo =
        min_bound.value_or(-std::numeric_limits<double>::infinity());
    const double hi =
        max_bound.value_or(std::numeric_limits<double>::infinity());
    return [draw = std::move(draw), lo, hi](std::mt19937_64& rng) {
        return std::clamp(draw(rng), lo, hi);
    };
}

std::optional<ModelSelection> select_best_model(
    const std::vector<FittedDistribution>& single_fits,
    const std::vector<FittedMixture>& mixtures) {
    std::optional<ModelSelection> best;
    for (const auto& f : single_fits) {
        if (!f.valid) continue;
        if (!best || f.bic < best->bic) {
            best = ModelSelection{BestModel{f}, f.bic,
                                  free_parameter_count(f.kind)};
        }
    }
    for (const auto& m : mixtures) {
        if (!m.valid) continue;
        if (!best || m.bic < best->bic) {
            best = ModelSelection{BestModel{m}, m.bic, free_parameter_count(m)};
        }
    }
    return best;
}

double pdf(const BestModel& m, double x) {
    return std::visit([x](const auto& v) { return pdf(v, x); }, m);
}

double cdf(const BestModel& m, double x) {
    return std::visit([x](const auto& v) { return cdf(v, x); }, m);
}

Sampler make_sampler(const BestModel& m, std::optional<double> min_bound,
                     std::optional<double> max_bound) {
    return std::visit(
        [&](const auto& v) { return make_sampler(v, min_bound, max_bound); },
        m);
}

}  // namespace dftracer::utils::utilities::common::statistics
