#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_AUGMENTATION_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_AUGMENTATION_H

#include <dftracer/utils/utilities/composites/dft/aggregators/aggregator_types.h>

#include <cmath>
#include <cstdint>

namespace dftracer::utils::utilities::composites::dft::aggregators {

struct AugmentationConfig {
    std::uint64_t source_interval_us;  // interval stored in index
    std::uint64_t target_interval_us;  // interval requested by user
};

// Augment a batch to match target interval.
// - If source > target: expand (split buckets, approximate with CI)
// - If source < target: shrink (merge buckets, lossless)
// - If source == target: pass through
AggregationBatch augment_batch(const AggregationBatch& input,
                               const AugmentationConfig& config);

// Compute Poisson 95% confidence interval for a count
inline CountConfidenceInterval compute_poisson_ci(double count,
                                                  double confidence = 1.96) {
    double sqrt_count = std::sqrt(count);
    return {std::max(0.0, count - confidence * sqrt_count),
            count + confidence * sqrt_count};
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_AUGMENTATION_H
