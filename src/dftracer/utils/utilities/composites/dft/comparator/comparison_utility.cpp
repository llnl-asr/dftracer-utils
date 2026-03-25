#include <dftracer/utils/utilities/composites/dft/comparator/comparison_utility.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::comparator {

namespace {

struct AggregationKeyEq {
    bool operator()(const AggregationKey& a,
                    const AggregationKey& b) const noexcept {
        return a == b;
    }
};

using KeySet =
    std::unordered_set<AggregationKey, AggregationKeyHash, AggregationKeyEq>;

// Helper: compute mean of a vector.
double vec_mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double sum = 0.0;
    for (double x : v) sum += x;
    return sum / static_cast<double>(v.size());
}

// Merge entries from a CollapsedMap into a single CollapsedMetrics.
// If cat_filter is non-empty, only include entries matching that cat.
CollapsedMetrics merge_collapsed(const CollapsedMap& cm,
                                 const std::string& cat_filter = "") {
    CollapsedMetrics total;
    std::vector<double> counts, dur_means, size_means, xfers, bws;
    for (const auto& [key, entry] : cm) {
        if (!cat_filter.empty() && key.cat != cat_filter) continue;
        total.merged.merge_from(entry.merged);
        counts.push_back(entry.count_mean);
        dur_means.push_back(entry.dur_mean_of_means);
        size_means.push_back(entry.size_mean_of_means);
        // Only include xfer/bw from ops that have them
        if (entry.xfer_mean > 0.0) xfers.push_back(entry.xfer_mean);
        if (entry.bw_mean > 0.0) bws.push_back(entry.bw_mean);
    }
    total.num_windows = dur_means.size();
    total.count_mean = vec_mean(counts);
    total.dur_mean_of_means = vec_mean(dur_means);
    total.size_mean_of_means = vec_mean(size_means);
    total.xfer_mean = vec_mean(xfers);
    total.bw_mean = vec_mean(bws);
    return total;
}

CollapsedMetrics merge_all(const CollapsedMap& cm) {
    return merge_collapsed(cm);
}

CollapsedMetrics merge_by_cat(const CollapsedMap& cm, const std::string& cat) {
    return merge_collapsed(cm, cat);
}

double worst_regression(const std::vector<MetricComparison>& metrics) {
    double worst = 0.0;
    for (const auto& mc : metrics) {
        if (mc.is_regression) {
            double abs_pct = std::abs(mc.pct_change);
            if (abs_pct > worst) worst = abs_pct;
        }
    }
    return worst;
}

GroupComparison make_group(const std::string& label,
                           const CollapsedMetrics& base_cm,
                           const CollapsedMetrics& var_cm, bool base_present,
                           bool var_present,
                           const std::vector<std::string>& metrics,
                           const std::vector<double>& percentiles) {
    GroupComparison gc;
    gc.label = label;
    gc.baseline_present = base_present;
    gc.variant_present = var_present;
    gc.metrics = compare_metrics(base_cm, var_cm, metrics, percentiles);
    gc.worst_pct_change = worst_regression(gc.metrics);
    return gc;
}

}  // namespace

// Build per-(cat,name) GroupComparison entries, grouped by cat.
// Returns a map: cat -> vector of (name, GroupComparison).
// Also fills all_cats in stable insertion order.
static std::map<std::string, std::vector<GroupComparison>> build_per_cat_groups(
    const CollapsedMap& base_collapsed, const CollapsedMap& var_collapsed,
    const std::vector<std::string>& metrics,
    const std::vector<double>& percentiles) {
    static const CollapsedMetrics EMPTY{};

    KeySet all_keys;
    all_keys.reserve(base_collapsed.size() + var_collapsed.size());
    for (const auto& [k, _] : base_collapsed) all_keys.insert(k);
    for (const auto& [k, _] : var_collapsed) all_keys.insert(k);

    // cat -> list of (name, GroupComparison)
    std::map<std::string, std::vector<GroupComparison>> by_cat;

    for (const auto& key : all_keys) {
        auto base_it = base_collapsed.find(key);
        auto var_it = var_collapsed.find(key);

        const CollapsedMetrics& base_cm =
            (base_it != base_collapsed.end()) ? base_it->second : EMPTY;
        const CollapsedMetrics& var_cm =
            (var_it != var_collapsed.end()) ? var_it->second : EMPTY;

        GroupComparison gc = make_group(
            key.name, base_cm, var_cm, base_it != base_collapsed.end(),
            var_it != var_collapsed.end(), metrics, percentiles);

        by_cat[key.cat].push_back(std::move(gc));
    }

    return by_cat;
}

std::vector<GroupComparison> ComparisonUtility::join_visitor(
    const ComparisonVisitorPair& pair) const {
    // Kept for compatibility; not used in the new build_result_tree path.
    auto base_collapsed = collapse_by_group(pair.baseline.aggregations);
    auto var_collapsed = collapse_by_group(pair.variant.aggregations);

    auto by_cat = build_per_cat_groups(base_collapsed, var_collapsed,
                                       pair.node.resolved_metrics,
                                       pair.node.resolved_percentiles);

    std::vector<GroupComparison> groups;
    for (auto& [cat, cat_groups] : by_cat) {
        for (auto& g : cat_groups) {
            groups.push_back(std::move(g));
        }
    }
    return groups;
}

GroupComparison ComparisonUtility::build_summary(
    const ComparisonVisitorPair& pair) const {
    auto base_collapsed = collapse_by_group(pair.baseline.aggregations);
    auto var_collapsed = collapse_by_group(pair.variant.aggregations);

    CollapsedMetrics base_total = merge_all(base_collapsed);
    CollapsedMetrics var_total = merge_all(var_collapsed);

    GroupComparison summary;
    summary.label = "";
    summary.baseline_present = !pair.baseline.aggregations.empty();
    summary.variant_present = !pair.variant.aggregations.empty();
    summary.metrics =
        compare_metrics(base_total, var_total, pair.node.resolved_metrics,
                        pair.node.resolved_percentiles);
    summary.worst_pct_change = worst_regression(summary.metrics);
    return summary;
}

void ComparisonUtility::sort_by_regression(
    std::vector<GroupComparison>& groups) const {
    std::sort(groups.begin(), groups.end(),
              [](const GroupComparison& a, const GroupComparison& b) {
                  return a.worst_pct_change > b.worst_pct_change;
              });
}

void ComparisonUtility::apply_threshold(std::vector<GroupComparison>& groups,
                                        double threshold_pct) const {
    if (threshold_pct <= 0.0) return;

    auto it =
        std::remove_if(groups.begin(), groups.end(),
                       [threshold_pct](const GroupComparison& g) {
                           for (const auto& mc : g.metrics) {
                               if (std::abs(mc.pct_change) >= threshold_pct)
                                   return false;
                           }
                           return true;
                       });
    groups.erase(it, groups.end());
}

NodeResult ComparisonUtility::build_result_tree(
    const ComparisonNode& node,
    const std::vector<ComparisonVisitorPair>& visitors,
    std::size_t& visitor_index) const {
    const ComparisonVisitorPair& pair = visitors[visitor_index++];

    auto base_collapsed = collapse_by_group(pair.baseline.aggregations);
    auto var_collapsed = collapse_by_group(pair.variant.aggregations);

    const auto& metrics = pair.node.resolved_metrics;
    const auto& percentiles = pair.node.resolved_percentiles;

    // Root summary: merge everything.
    CollapsedMetrics base_total = merge_all(base_collapsed);
    CollapsedMetrics var_total = merge_all(var_collapsed);

    NodeResult result;
    result.name = node.name;
    result.composed_query = node.composed_query;
    result.group_by = node.group_by;
    result.groups = {};  // detail lives in children

    result.summary.label = "";
    result.summary.baseline_present = !pair.baseline.aggregations.empty();
    result.summary.variant_present = !pair.variant.aggregations.empty();
    result.summary.metrics =
        compare_metrics(base_total, var_total, metrics, percentiles);
    result.summary.worst_pct_change = worst_regression(result.summary.metrics);

    // Build per-category children.
    auto by_cat = build_per_cat_groups(base_collapsed, var_collapsed, metrics,
                                       percentiles);

    for (auto& [cat, cat_groups] : by_cat) {
        NodeResult child;
        child.name = cat;
        child.composed_query = node.composed_query;
        child.group_by = node.group_by;

        // Category summary: merge all entries for this cat.
        CollapsedMetrics base_cat = merge_by_cat(base_collapsed, cat);
        CollapsedMetrics var_cat = merge_by_cat(var_collapsed, cat);

        child.summary.label = "";
        child.summary.baseline_present =
            base_cat.merged.count > 0 || base_cat.num_windows > 0;
        child.summary.variant_present =
            var_cat.merged.count > 0 || var_cat.num_windows > 0;
        child.summary.metrics =
            compare_metrics(base_cat, var_cat, metrics, percentiles);
        child.summary.worst_pct_change =
            worst_regression(child.summary.metrics);

        // Per-operation groups within this category.
        sort_by_regression(cat_groups);
        apply_threshold(cat_groups, node.resolved_threshold_pct);
        child.groups = std::move(cat_groups);

        result.children.push_back(std::move(child));
    }

    // Recurse into config children (sub-queries).
    for (const auto& cfg_child : node.children) {
        result.children.push_back(
            build_result_tree(cfg_child, visitors, visitor_index));
    }

    return result;
}

coro::CoroTask<ComparisonUtilityOutput> ComparisonUtility::process(
    const ComparisonUtilityInput& input) {
    ComparisonUtilityOutput output;
    std::size_t visitor_index = 0;
    output.result =
        build_result_tree(input.root_node, input.visitors, visitor_index);
    output.success = true;
    co_return output;
}

}  // namespace dftracer::utils::utilities::composites::dft::comparator
