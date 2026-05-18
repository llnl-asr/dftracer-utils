#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_COMPARATOR_COMPARISON_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_COMPARATOR_COMPARISON_UTILITY_H

#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_output.h>
#include <dftracer/utils/utilities/composites/dft/comparator/comparison_config.h>
#include <dftracer/utils/utilities/composites/dft/comparator/comparison_result.h>

#include <vector>

namespace dftracer::utils::utilities::composites::dft::comparator {

using aggregators::EventAggregatorOutput;

/// Paired baseline/variant aggregation outputs for a single comparison
/// node.
struct ComparisonVisitorPair {
    /// Aggregation output for the baseline run.
    EventAggregatorOutput baseline;
    /// Aggregation output for the variant run.
    EventAggregatorOutput variant;
    /// Resolved config node for this visitor.
    ComparisonNode node;
};

/// Input to ComparisonUtility::process().
struct ComparisonUtilityInput {
    /// Visitor pairs, one per flattened node in the tree.
    std::vector<ComparisonVisitorPair> visitors;
    /// Root node of the comparison tree (for hierarchy reconstruction).
    ComparisonNode root_node;

    /// Number of baseline trace files (for metadata).
    std::size_t baseline_file_count = 0;
    /// Number of variant trace files (for metadata).
    std::size_t variant_file_count = 0;
};

/// Output from ComparisonUtility::process().
struct ComparisonUtilityOutput {
    /// Hierarchical comparison result tree.
    NodeResult result;
    /// Whether the comparison completed successfully.
    bool success = false;
};

/// Joins baseline and variant aggregation outputs, builds the
/// hierarchical comparison tree (root -> categories -> operations),
/// and computes deltas with Cohen's d significance classification.
class ComparisonUtility : public utilities::Utility<ComparisonUtilityInput,
                                                    ComparisonUtilityOutput> {
   public:
    /// Run the comparison pipeline.
    coro::CoroTask<ComparisonUtilityOutput> process(
        const ComparisonUtilityInput& input) override;

   private:
    /// Join a single visitor pair into per-group comparisons.
    std::vector<GroupComparison> join_visitor(
        const ComparisonVisitorPair& pair) const;

    /// Build an aggregate summary across all groups for a visitor pair.
    GroupComparison build_summary(const ComparisonVisitorPair& pair) const;

    /// Sort groups by worst regression first.
    void sort_by_regression(std::vector<GroupComparison>& groups) const;

    /// Remove groups below the threshold percentage.
    void apply_threshold(std::vector<GroupComparison>& groups,
                         double threshold_pct) const;

    /// Recursively build the result tree from flattened visitors.
    NodeResult build_result_tree(
        const ComparisonNode& node,
        const std::vector<ComparisonVisitorPair>& visitors,
        std::size_t& visitor_index) const;
};

}  // namespace dftracer::utils::utilities::composites::dft::comparator

#endif
