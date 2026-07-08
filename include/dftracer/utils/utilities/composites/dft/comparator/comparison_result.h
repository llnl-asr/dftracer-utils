#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_COMPARATOR_COMPARISON_RESULT_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_COMPARATOR_COMPARISON_RESULT_H

#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_key.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_map.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_metrics.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>
#endif

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::comparator {

using aggregators::AggregationKey;
using aggregators::AggregationKeyEqual;
using aggregators::AggregationKeyHash;
using aggregators::AggregationMap;
using aggregators::AggregationMetrics;
using aggregators::MetricStats;

/// Metric-name classification, shared by the Arrow exporter and the tree
/// formatter so the atomic-name set cannot drift between them. Atomic names are
/// standalone metrics (e.g. "count"); others split into group + leaf, e.g.
/// "dur_mean" -> group "dur", leaf "mean".
bool is_atomic_metric(const std::string& name);
std::string metric_group(const std::string& name);
std::string metric_leaf(const std::string& name);

/// Cohen's d effect size classification.
enum class Significance : int {
    NEGLIGIBLE = 0,  ///< |d| <= 0.2
    SMALL = 1,       ///< |d| > 0.2
    MEDIUM = 2,      ///< |d| > 0.5
    LARGE = 3        ///< |d| > 0.8
};

/// Human-readable name of an effect-size bucket (e.g. "MEDIUM").
const char* significance_to_string(Significance s);

/// Comparison of a single metric between baseline and variant.
struct MetricComparison {
    /// Metric name (e.g. "count", "dur_mean", "dur_p50", "size").
    std::string metric_name;
    /// Baseline value (mean across time windows).
    double baseline_value = 0.0;
    /// Variant value (mean across time windows).
    double variant_value = 0.0;
    /// Baseline standard deviation across time windows (0 if N<=1).
    double baseline_stdev = 0.0;
    /// Variant standard deviation across time windows.
    double variant_stdev = 0.0;
    /// Absolute difference (variant - baseline).
    double delta = 0.0;
    /// Percentage change (e.g. 15.3 for +15.3%).
    double pct_change = 0.0;
    /// Cohen's d effect size.
    double cohens_d = 0.0;
    /// Classified significance level.
    Significance significance = Significance::NEGLIGIBLE;
    /// True if performance got worse (higher duration, lower bandwidth).
    bool is_regression = false;
};

/// Comparison of all metrics for a single (category, operation) group.
struct GroupComparison {
    /// Group label (e.g. "POSIX/read", empty for summary).
    std::string label;
    /// Whether this group exists in the baseline.
    bool baseline_present = true;
    /// Whether this group exists in the variant.
    bool variant_present = true;
    /// Per-metric comparisons for this group.
    std::vector<MetricComparison> metrics;
    /// Worst percentage change across all metrics (for regression sorting).
    double worst_pct_change = 0.0;
};

/// Result for a single node in the comparison tree.
struct NodeResult {
    /// Node name from the config.
    std::string name;
    /// Full composed query for this node.
    std::string composed_query;
    /// Group-by keys used.
    std::vector<std::string> group_by;
    /// Per-group comparisons (empty if summary-only).
    std::vector<GroupComparison> groups;
    /// Aggregate summary across all groups (always present).
    GroupComparison summary;
    /// Child node results.
    std::vector<NodeResult> children;
};

/// Metadata extracted from a trace run (baseline or variant).
struct TraceMetadata {
    /// Distinct data files accessed (unique fhash); falls back to trace-file
    /// count when no fhash is present.
    std::size_t file_count = 0;
    /// Per-process file accesses (distinct pid + fhash). Equals file_count for
    /// file-per-process workloads; larger when processes share files.
    std::size_t proc_file_count = 0;
    /// Number of unique process IDs.
    std::size_t process_count = 0;
    /// Number of unique thread IDs.
    std::size_t thread_count = 0;
    /// Total bytes transferred (I/O operations only).
    double total_bytes = 0.0;
    /// Sum of all event durations in microseconds.
    double total_io_time_us = 0.0;
    /// Wall-clock makespan in microseconds (max ts - min ts).
    double makespan_us = 0.0;
};

/// Top-level output of the comparison pipeline.
struct ComparisonOutput {
    /// Baseline file or directory path.
    std::string baseline_path;
    /// Variant file or directory path.
    std::string variant_path;
    /// Number of baseline trace files processed.
    std::size_t baseline_file_count = 0;
    /// Number of variant trace files processed.
    std::size_t variant_file_count = 0;
    /// Metadata extracted from baseline traces.
    TraceMetadata baseline_meta;
    /// Metadata extracted from variant traces.
    TraceMetadata variant_meta;
    /// Top-level comparison tree results.
    std::vector<NodeResult> nodes;
    /// Total pipeline execution time in milliseconds.
    double execution_time_ms = 0.0;

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    /// Flatten the tree into a single Arrow record batch.
    ///
    /// Columns: node_path, metric_group, metric_name, baseline,
    /// variant, baseline_stdev, variant_stdev, delta, pct_change,
    /// cohens_d, significance, is_regression.
    /// Rows with all-zero values are skipped.
    common::arrow::ArrowExportResult to_arrow() const;
#endif
};

/// Per-(category, operation) collapsed metrics aggregated across
/// processes and time windows.
///
/// Within each time window: max across processes.
/// Across windows: mean +/- stdev of per-window max values.
struct CollapsedMetrics {
    /// DDSketch merged across all windows for percentile queries.
    AggregationMetrics merged{0.01};
    /// Mean of per-window max event counts.
    double count_mean = 0.0;
    /// Mean of per-window mean durations.
    double dur_mean_of_means = 0.0;
    /// Stdev of per-window mean durations.
    double dur_stdev_of_means = 0.0;
    /// Mean of per-window mean sizes.
    double size_mean_of_means = 0.0;
    /// Stdev of per-window mean sizes.
    double size_stdev_of_means = 0.0;
    /// Mean of per-window max transfer sizes.
    double xfer_mean = 0.0;
    /// Stdev of per-window max transfer sizes.
    double xfer_stdev = 0.0;
    /// Mean of per-window max bandwidths.
    double bw_mean = 0.0;
    /// Stdev of per-window max bandwidths.
    double bw_stdev = 0.0;
    /// Number of time windows contributing to these statistics.
    std::size_t num_windows = 0;
};

/// Map from (category, operation) key to collapsed metrics.
using CollapsedMap =
    std::unordered_map<AggregationKey, CollapsedMetrics, AggregationKeyHash,
                       AggregationKeyEqual>;

/// Extract trace metadata (unique PIDs, TIDs, makespan) from raw
/// aggregation output.
TraceMetadata extract_metadata(const AggregationMap& aggregations,
                               std::size_t file_count);

/// Collapse aggregation output by grouping on (category, name) and
/// merging across time windows.
CollapsedMap collapse_by_group(const AggregationMap& aggregations);

/// Compute Cohen's d effect size from two MetricStats with sample
/// counts.
double compute_cohens_d(const MetricStats& baseline, std::uint64_t n_base,
                        const MetricStats& variant, std::uint64_t n_var);

/// Classify a Cohen's d value into a Significance level.
Significance classify_significance(double cohens_d);

/// Build MetricComparison entries for trace-level metadata
/// (file_count, process_count, thread_count, makespan, total_bytes,
/// total_io_time). Intended for injection into the root SUMMARY.
std::vector<MetricComparison> build_metadata_metrics(
    const TraceMetadata& baseline, const TraceMetadata& variant);

/// Build MetricComparison entries for a pair of CollapsedMetrics,
/// comparing the requested metrics and percentiles.
std::vector<MetricComparison> compare_metrics(
    const CollapsedMetrics& baseline, const CollapsedMetrics& variant,
    const std::vector<std::string>& metrics,
    const std::vector<double>& percentiles);

}  // namespace dftracer::utils::utilities::composites::dft::comparator

#endif
