#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/common/statistics/distributions.h>
#include <dftracer/utils/utilities/common/statistics/mixture.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <variant>
#include <vector>

using namespace dftracer::utils::utilities::common::statistics;

namespace {

// Sample n points from a K-component Normal mixture with given
// weights/means/stddevs.
std::vector<double> sample_mixture(std::size_t n,
                                   const std::vector<double>& weights,
                                   const std::vector<double>& means,
                                   const std::vector<double>& stddevs,
                                   std::uint64_t seed = 4242) {
    std::mt19937_64 rng(seed);
    std::discrete_distribution<int> cat(weights.begin(), weights.end());
    std::vector<double> data;
    data.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const int k = cat(rng);
        data.push_back(
            std::normal_distribution<double>(means[k], stddevs[k])(rng));
    }
    return data;
}

}  // namespace

TEST_SUITE("fit_gaussian_mixture") {
    TEST_CASE("K=2: recovers means within tolerance") {
        // Two well-separated Gaussians: N(0, 0.3) with weight 0.4, N(3, 0.5)
        // with 0.6.
        auto data = sample_mixture(5000, {0.4, 0.6}, {0.0, 3.0}, {0.3, 0.5},
                                   /*seed=*/7);
        const auto fit = fit_gaussian_mixture(data, 2);
        REQUIRE(fit.valid);
        REQUIRE(fit.components.size() == 2);

        // Sort by mean for deterministic comparison (EM has label-switching
        // freedom).
        std::vector<std::size_t> idx{0, 1};
        std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
            return fit.components[a].mean < fit.components[b].mean;
        });
        const auto& c0 = fit.components[idx[0]];
        const auto& c1 = fit.components[idx[1]];

        CHECK(c0.mean == doctest::Approx(0.0).epsilon(0.1));
        CHECK(c1.mean == doctest::Approx(3.0).epsilon(0.05));
        CHECK(c0.stddev == doctest::Approx(0.3).epsilon(0.15));
        CHECK(c1.stddev == doctest::Approx(0.5).epsilon(0.15));
        CHECK(fit.weights[idx[0]] == doctest::Approx(0.4).epsilon(0.1));
        CHECK(fit.weights[idx[1]] == doctest::Approx(0.6).epsilon(0.1));
        CHECK(fit.converged);
    }

    TEST_CASE("K=3: converges and weights sum to 1") {
        auto data = sample_mixture(6000, {0.3, 0.4, 0.3}, {-2.0, 0.5, 3.0},
                                   {0.4, 0.3, 0.5},
                                   /*seed=*/11);
        const auto fit = fit_gaussian_mixture(data, 3);
        REQUIRE(fit.valid);
        REQUIRE(fit.components.size() == 3);

        double wsum = 0.0;
        for (double w : fit.weights) wsum += w;
        CHECK(wsum == doctest::Approx(1.0).epsilon(1e-9));
        CHECK(fit.iterations > 0);
    }

    TEST_CASE("rejects too few samples") {
        std::vector<double> data{1.0, 2.0};
        const auto fit = fit_gaussian_mixture(data, 2);
        CHECK_FALSE(fit.valid);
    }
}

TEST_SUITE("FittedMixture pdf/cdf/sampler") {
    TEST_CASE("pdf matches hand-computed value") {
        FittedMixture m;
        m.weights = {0.5, 0.5};
        m.components = {{0.0, 1.0}, {2.0, 1.0}};
        m.valid = true;
        // pdf at x=1: 0.5 * N(1; 0, 1) + 0.5 * N(1; 2, 1)
        // = 0.5 * 0.24197 + 0.5 * 0.24197 = 0.24197
        CHECK(pdf(m, 1.0) == doctest::Approx(0.24197).epsilon(1e-4));
    }

    TEST_CASE("cdf is monotone and bounded") {
        FittedMixture m;
        m.weights = {0.3, 0.7};
        m.components = {{-1.0, 0.5}, {1.0, 0.5}};
        m.valid = true;
        CHECK(cdf(m, -10.0) == doctest::Approx(0.0).epsilon(1e-6));
        CHECK(cdf(m, 10.0) == doctest::Approx(1.0).epsilon(1e-6));
        CHECK(cdf(m, 0.0) > cdf(m, -1.0));
    }

    TEST_CASE("sampler reproduces mixture mean within tolerance") {
        FittedMixture m;
        m.weights = {0.5, 0.5};
        m.components = {{1.0, 0.2}, {3.0, 0.2}};
        m.valid = true;
        auto sampler = make_sampler(m);
        std::mt19937_64 rng(17);
        double sum = 0.0;
        const int n = 6000;
        for (int i = 0; i < n; ++i) sum += sampler(rng);
        // True mixture mean = 0.5*1 + 0.5*3 = 2.0
        CHECK(sum / n == doctest::Approx(2.0).epsilon(0.05));
    }
}

TEST_SUITE("select_best_model") {
    TEST_CASE("picks mixture for bimodal data") {
        auto data = sample_mixture(4000, {0.5, 0.5}, {0.0, 4.0}, {0.3, 0.4},
                                   /*seed=*/22);
        const auto singles = fit_all_single_distributions(data);
        std::vector<FittedMixture> mixes{fit_gaussian_mixture(data, 2),
                                         fit_gaussian_mixture(data, 3)};
        const auto best = select_best_model(singles, mixes);
        REQUIRE(best.has_value());
        // For clearly bimodal data, no single dist should win.
        CHECK(std::holds_alternative<FittedMixture>(best->model));
    }

    TEST_CASE("picks single for unimodal Normal data") {
        std::mt19937_64 rng(33);
        std::vector<double> data;
        data.reserve(3000);
        for (int i = 0; i < 3000; ++i)
            data.push_back(std::normal_distribution<double>(2.0, 0.5)(rng));
        const auto singles = fit_all_single_distributions(data);
        std::vector<FittedMixture> mixes{fit_gaussian_mixture(data, 2),
                                         fit_gaussian_mixture(data, 3)};
        const auto best = select_best_model(singles, mixes);
        REQUIRE(best.has_value());
        // With BIC's parameter penalty, a single Normal should beat GMM-2/3.
        CHECK(std::holds_alternative<FittedDistribution>(best->model));
    }

    TEST_CASE("empty inputs return nullopt") {
        const auto best = select_best_model({}, {});
        CHECK_FALSE(best.has_value());
    }
}

TEST_SUITE("BestModel variant dispatch") {
    TEST_CASE("pdf dispatches through variant") {
        FittedDistribution f{
            DistributionKind::Normal, {0.0, 1.0, 0.0}, 0.0, 0.0, 0.0, true};
        BestModel bm{f};
        // pdf(std normal, 0) = 1/sqrt(2*pi) ~ 0.39894
        CHECK(pdf(bm, 0.0) == doctest::Approx(0.39894).epsilon(1e-3));
    }

    TEST_CASE("sampler dispatches through variant") {
        FittedMixture m;
        m.weights = {1.0};
        m.components = {{5.0, 0.1}};
        m.valid = true;
        BestModel bm{m};
        auto s = make_sampler(bm);
        std::mt19937_64 rng(55);
        const double draw = s(rng);
        CHECK(draw == doctest::Approx(5.0).epsilon(0.1));
    }
}
