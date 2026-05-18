#include <dftracer/utils/utilities/dlio/optimizer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace dftracer::utils::utilities::dlio {

namespace stats = ::dftracer::utils::utilities::common::statistics;

double percentile(const std::vector<double>& sorted_data, double pct) {
    if (sorted_data.empty()) return 0.0;
    const double p = std::clamp(pct, 0.0, 100.0) / 100.0;
    const double idx = p * static_cast<double>(sorted_data.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(idx));
    const auto hi = static_cast<std::size_t>(std::ceil(idx));
    if (lo == hi) return sorted_data[lo];
    const double frac = idx - static_cast<double>(lo);
    return sorted_data[lo] * (1.0 - frac) + sorted_data[hi] * frac;
}

OptimizerResult optimize_max_bound_percentile(
    const BarrierSimulatorContext& context, const BestModel& model,
    std::vector<double> sample_times, const OptimizerOptions& options) {
    OptimizerResult out;
    out.best_percentile = options.initial_percentile;

    std::sort(sample_times.begin(), sample_times.end());
    if (sample_times.empty()) return out;

    const double sample_min = sample_times.front();

    BarrierSimulator sim;
    double current_percentile = options.initial_percentile;
    double velocity = 0.0;
    double best_e2e_error = std::numeric_limits<double>::infinity();
    int iterations_without_improvement = 0;

    constexpr double kImprovementThreshold = 0.001;  // 0.1% relative

    for (int iter = 0; iter < options.max_iterations; ++iter) {
        const double max_bound = percentile(sample_times, current_percentile);
        auto sampler = stats::make_sampler(model, sample_min, max_bound);

        auto result = sim.simulate(context, options.base_seed, sampler);
        out.iterations_used = iter + 1;

        // Track best result by E2E error (improvement must beat threshold).
        const bool first = (iter == 0);
        const bool better =
            result.e2e_error < best_e2e_error * (1.0 - kImprovementThreshold);
        if (first || better) {
            out.best = result;
            out.best_percentile = current_percentile;
            best_e2e_error = result.e2e_error;
            iterations_without_improvement = 0;
        } else {
            ++iterations_without_improvement;
        }

        const bool e2e_ok = result.e2e_error < options.target_e2e_error;
        const bool cdf_ok =
            result.fetch_block_cdf_similarity > options.target_cdf_similarity;
        if (e2e_ok && cdf_ok) {
            out.converged = true;
            return out;
        }
        if (iterations_without_improvement >= options.patience) return out;

        // Momentum-smoothed step. Overshooting -> shrink percentile;
        // undershooting -> grow it; close-but-not-converged -> nudge by CDF.
        double step = 0.0;
        if (result.e2e_duration > context.trace_e2e_duration) {
            const double aggressive = result.e2e_error > 0.10 ? 2.0 : 1.0;
            step = -options.epsilon * aggressive;
        } else if (result.e2e_duration < context.trace_e2e_duration * 0.95) {
            step = options.epsilon * 0.5;
        } else if (result.fetch_block_cdf_similarity <
                   options.target_cdf_similarity) {
            step = -options.epsilon * 0.5;
        }
        velocity = options.momentum * velocity + step;
        current_percentile = std::clamp(current_percentile + velocity,
                                        options.min_percentile, 100.0);
    }

    return out;
}

}  // namespace dftracer::utils::utilities::dlio
