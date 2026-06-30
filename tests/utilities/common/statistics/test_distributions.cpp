#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/utilities/common/statistics/distributions.h>
#include <doctest/doctest.h>

#include <cmath>
#include <functional>
#include <random>
#include <vector>

using namespace dftracer::utils::utilities::common::statistics;
using Rng = std::mt19937_64;

namespace {

std::vector<double> generate_samples(std::size_t n,
                                     std::function<double(std::mt19937_64&)> f,
                                     std::uint64_t seed = 12345) {
    std::mt19937_64 rng(seed);
    std::vector<double> data;
    data.reserve(n);
    for (std::size_t i = 0; i < n; ++i) data.push_back(f(rng));
    return data;
}

}  // namespace

TEST_SUITE("fit_single_distribution") {
    TEST_CASE("Normal: recovers mean and stddev") {
        auto data = generate_samples(5000, [](auto& r) {
            return std::normal_distribution<double>(2.5, 0.7)(r);
        });
        const auto fit =
            fit_single_distribution(DistributionKind::Normal, data);
        REQUIRE(fit.valid);
        CHECK(fit.params[0] == doctest::Approx(2.5).epsilon(0.05));
        CHECK(fit.params[1] == doctest::Approx(0.7).epsilon(0.05));
        // Same-family fit should have small KS statistic on 5k samples.
        CHECK(fit.ks_stat < 0.05);
    }

    TEST_CASE("Lognormal: recovers mu and sigma in log space") {
        auto data = generate_samples(5000, [](auto& r) {
            return std::lognormal_distribution<double>(-1.0, 0.5)(r);
        });
        const auto fit =
            fit_single_distribution(DistributionKind::Lognormal, data);
        REQUIRE(fit.valid);
        CHECK(fit.params[0] == doctest::Approx(-1.0).epsilon(0.05));
        CHECK(fit.params[1] == doctest::Approx(0.5).epsilon(0.05));
        CHECK(fit.ks_stat < 0.05);
    }

    TEST_CASE("Exponential: recovers rate") {
        auto data = generate_samples(5000, [](auto& r) {
            return std::exponential_distribution<double>(3.0)(r);
        });
        const auto fit =
            fit_single_distribution(DistributionKind::Exponential, data);
        REQUIRE(fit.valid);
        CHECK(fit.params[0] == doctest::Approx(3.0).epsilon(0.05));
        CHECK(fit.ks_stat < 0.05);
    }

    TEST_CASE("Gamma: recovers shape and scale within 5%") {
        auto data = generate_samples(5000, [](auto& r) {
            return std::gamma_distribution<double>(2.0, 0.3)(r);
        });
        const auto fit = fit_single_distribution(DistributionKind::Gamma, data);
        REQUIRE(fit.valid);
        CHECK(fit.params[0] == doctest::Approx(2.0).epsilon(0.05));
        CHECK(fit.params[1] == doctest::Approx(0.3).epsilon(0.05));
        CHECK(fit.ks_stat < 0.05);
    }

    TEST_CASE("Weibull: recovers shape and scale within 5%") {
        auto data = generate_samples(5000, [](auto& r) {
            return std::weibull_distribution<double>(1.5, 2.0)(r);
        });
        const auto fit =
            fit_single_distribution(DistributionKind::Weibull, data);
        REQUIRE(fit.valid);
        CHECK(fit.params[0] == doctest::Approx(1.5).epsilon(0.05));
        CHECK(fit.params[1] == doctest::Approx(2.0).epsilon(0.05));
        CHECK(fit.ks_stat < 0.05);
    }

    TEST_CASE("Lognormal: rejects non-positive data") {
        std::vector<double> data{1.0, -2.0, 3.0, 4.0};
        const auto fit =
            fit_single_distribution(DistributionKind::Lognormal, data);
        CHECK_FALSE(fit.valid);
    }

    TEST_CASE("Normal: rejects too few samples") {
        std::vector<double> data{1.0};
        const auto fit =
            fit_single_distribution(DistributionKind::Normal, data);
        CHECK_FALSE(fit.valid);
    }
}

TEST_SUITE("fit_all_single_distributions") {
    TEST_CASE("ranks correct family at the top") {
        auto data = generate_samples(2000, [](auto& r) {
            return std::lognormal_distribution<double>(0.0, 0.4)(r);
        });
        const auto fits = fit_all_single_distributions(data);
        REQUIRE_FALSE(fits.empty());
        const auto best = best_fit_by_ks(fits);
        REQUIRE(best.has_value());
        CHECK(best->kind == DistributionKind::Lognormal);
    }

    TEST_CASE("all valid fits sorted ascending by KS") {
        auto data = generate_samples(2000, [](auto& r) {
            return std::gamma_distribution<double>(2.0, 0.5)(r);
        });
        const auto fits = fit_all_single_distributions(data);
        REQUIRE(fits.size() == 5);
        double last_ks = -1.0;
        for (const auto& f : fits) {
            if (!f.valid) break;
            CHECK(f.ks_stat >= last_ks);
            last_ks = f.ks_stat;
        }
    }
}

TEST_SUITE("FittedDistribution pdf/cdf/quantile") {
    TEST_CASE("Normal cdf matches Gaussian quantiles") {
        FittedDistribution fit{
            DistributionKind::Normal, {0.0, 1.0, 0.0}, 0.0, 0.0, 0.0, true};
        // 97.5th percentile of standard normal ~ 1.96.
        CHECK(quantile(fit, 0.975) == doctest::Approx(1.959964).epsilon(1e-3));
        CHECK(cdf(fit, 0.0) == doctest::Approx(0.5));
    }

    TEST_CASE("Lognormal quantile at median") {
        FittedDistribution fit{
            DistributionKind::Lognormal, {0.0, 1.0, 0.0}, 0.0, 0.0, 0.0, true};
        CHECK(quantile(fit, 0.5) == doctest::Approx(1.0).epsilon(1e-6));
    }
}

TEST_SUITE("make_sampler") {
    TEST_CASE("Normal sampler reproduces fit mean within tolerance") {
        FittedDistribution fit{
            DistributionKind::Normal, {1.0, 0.2, 0.0}, 0.0, 0.0, 0.0, true};
        auto sampler = make_sampler(fit);
        Rng rng(99);
        double sum = 0.0;
        const int n = 4000;
        for (int i = 0; i < n; ++i) sum += sampler(rng);
        const double mean = sum / n;
        CHECK(mean == doctest::Approx(1.0).epsilon(0.05));
    }

    TEST_CASE("clamps to provided bounds") {
        FittedDistribution fit{
            DistributionKind::Normal, {0.0, 1.0, 0.0}, 0.0, 0.0, 0.0, true};
        auto sampler = make_sampler(fit, /*min_bound=*/-0.5, /*max_bound=*/0.5);
        Rng rng(7);
        for (int i = 0; i < 200; ++i) {
            const double s = sampler(rng);
            CHECK(s >= -0.5);
            CHECK(s <= 0.5);
        }
    }

    TEST_CASE("throws on invalid fit") {
        FittedDistribution fit;  // valid = false
        CHECK_THROWS_AS(make_sampler(fit), dftracer::utils::DFTUtilsException);
    }
}
