// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DOMAINS_ANALYSIS_COMPARATOR_COMPARISON_UTILITY_H
#define DFTRACER_UTILS_DOMAINS_ANALYSIS_COMPARATOR_COMPARISON_UTILITY_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/domains/analysis/comparator/comparison_config.h>
#include <dftracer/utils/domains/analysis/comparator/comparison_result.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>

namespace dftracer::utils::domains::analysis::comparator {

/// Paired baseline/variant collapsed groups for a single comparison node.
struct ComparisonVisitorPair {
    /// Groups of the baseline run; not owned, and they may be the same as
    /// `variant` when a trace is compared with itself.
    const CollapsedMap* baseline = nullptr;
    /// Groups of the variant run; not owned.
    const CollapsedMap* variant = nullptr;
    /// Resolved config node for this visitor.
    ComparisonNode node;
};

/// Input to ComparisonUtility::process().
struct ComparisonUtilityInput {
    /// Visitor pairs, one per flattened node in the tree.
    dstd::Vector<ComparisonVisitorPair> visitors;
    /// Root node of the comparison tree (for hierarchy reconstruction).
    ComparisonNode root_node;

    /// The table that resolves the ids of every group of `visitors`; it
    /// must outlive the call.
    const dstd::StringIntern* strings = nullptr;

    /// Number of baseline trace files (for metadata).
    std::size_t baseline_file_count = 0;
    /// Number of variant trace files (for metadata).
    std::size_t variant_file_count = 0;
};

/// Success payload from ComparisonUtility::process(); failures are
/// reported via dstd::Result<ComparisonUtilityOutput>.
struct ComparisonUtilityOutput {
    /// Hierarchical comparison result tree.
    NodeResult result;
};

/// Joins baseline and variant aggregation outputs, builds the
/// hierarchical comparison tree (root -> categories -> operations),
/// and computes deltas with Cohen's d significance classification.
class ComparisonUtility {
   public:
    /// Run the comparison pipeline.
    coro::CoroTask<dstd::Result<ComparisonUtilityOutput>> operator()(
        const ComparisonUtilityInput& input);

   private:
    /// Sort groups by worst regression first.
    void sort_by_regression(dstd::Vector<GroupComparison>& groups) const;

    /// Remove groups below the threshold percentage.
    void apply_threshold(dstd::Vector<GroupComparison>& groups,
                         double threshold_pct) const;

    /// Recursively build the result tree from flattened visitors.
    NodeResult build_result_tree(
        const ComparisonNode& node,
        const dstd::Vector<ComparisonVisitorPair>& visitors,
        const dstd::StringIntern& intern, std::size_t& visitor_index) const;
};

}  // namespace dftracer::utils::domains::analysis::comparator

#endif
