#ifndef DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_MIXTURE_H
#define DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_MIXTURE_H

#include <dftracer/utils/utilities/common/statistics/distributions.h>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace dftracer::utils::utilities::common::statistics {

struct GmmComponent {
    double mean = 0.0;
    double stddev = 0.0;
};

// Univariate Gaussian Mixture Model fit. Component count is `weights.size()`.
struct FittedMixture {
    std::vector<double> weights;           // sum to 1
    std::vector<GmmComponent> components;  // same length as weights
    double log_likelihood = 0.0;
    double bic = 0.0;
    int iterations = 0;
    bool converged = false;
    bool valid = false;
};

struct MixtureFitOptions {
    int max_iter = 200;
    double tol = 1e-6;
    double variance_floor = 1e-12;  // prevent component collapse
    std::uint64_t seed = 0xC0FFEE;
};

// Fits a K-component Gaussian Mixture via EM. K-means-style initialization on
// quantile-spread means and total-variance / K for each component.
FittedMixture fit_gaussian_mixture(const std::vector<double>& data, int K,
                                   const MixtureFitOptions& options = {});

double pdf(const FittedMixture& mix, double x);
double cdf(const FittedMixture& mix, double x);

// Free-parameter count for BIC: 3K - 1  (K means + K stddevs + K-1 free
// weights).
int free_parameter_count(const FittedMixture& mix);

// Sampler from a fitted mixture. Draws a component by weight then a Normal.
Sampler make_sampler(const FittedMixture& mix,
                     std::optional<double> min_bound = std::nullopt,
                     std::optional<double> max_bound = std::nullopt);

using BestModel = std::variant<FittedDistribution, FittedMixture>;

struct ModelSelection {
    BestModel model;
    double bic = 0.0;
    int free_params = 0;
};

// Selects the lowest-BIC model among the candidates. `single_fits` is typically
// the output of fit_all_single_distributions(); `mixtures` is typically two
// entries (GMM-2 and GMM-3). Invalid fits are ignored.
std::optional<ModelSelection> select_best_model(
    const std::vector<FittedDistribution>& single_fits,
    const std::vector<FittedMixture>& mixtures);

double pdf(const BestModel& m, double x);
double cdf(const BestModel& m, double x);
Sampler make_sampler(const BestModel& m,
                     std::optional<double> min_bound = std::nullopt,
                     std::optional<double> max_bound = std::nullopt);

}  // namespace dftracer::utils::utilities::common::statistics

#endif
