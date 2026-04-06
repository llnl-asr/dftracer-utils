#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_COMPARATOR_COMPARISON_CONFIG_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_COMPARATOR_COMPARISON_CONFIG_H

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::comparator {

/// Default settings inherited by all comparison nodes unless overridden.
struct ComparisonDefaults {
    /// Metric groups to compare (count, duration, size, transfer_size,
    /// bandwidth).
    std::vector<std::string> metrics = {"count", "duration", "size",
                                        "transfer_size", "bandwidth"};
    /// Percentiles to compute from DDSketch (e.g. p50, p95, p99).
    std::vector<double> percentiles = {0.50, 0.95, 0.99};
    /// Hide changes below this percentage in the output.
    double threshold_pct = 0.0;
    /// Time bucket width in milliseconds for aggregation windows.
    double time_interval_ms = 5000.0;
    /// Sort order for groups: "regression" sorts worst regressions first.
    std::string sort_by = "regression";
};

/// A node in the hierarchical comparison tree.
///
/// Each node carries a query that is AND'd with its parent's query,
/// and optional per-node overrides for metrics, percentiles, and
/// threshold. Children inherit from their parent unless they override.
struct ComparisonNode {
    /// Display name for this node (e.g. "POSIX I/O", "reads").
    std::string name;
    /// Additional query filter, AND'd with the parent's composed query.
    std::string query;
    /// Extra group-by keys beyond the default (cat, name).
    std::vector<std::string> group_by;

    /// Per-node metric override (nullopt = inherit from parent).
    std::optional<std::vector<std::string>> metrics;
    /// Per-node percentile override (nullopt = inherit from parent).
    std::optional<std::vector<double>> percentiles;
    /// Per-node threshold override (nullopt = inherit from parent).
    std::optional<double> threshold_pct;
    /// Per-node sort override (nullopt = inherit from parent).
    std::optional<std::string> sort_by;

    /// Child nodes forming the comparison hierarchy.
    std::vector<ComparisonNode> children;

    /// Full query including all ancestor queries, populated by
    /// resolve().
    std::string composed_query;
    /// Resolved metrics after inheritance, populated by resolve().
    std::vector<std::string> resolved_metrics;
    /// Resolved percentiles after inheritance, populated by resolve().
    std::vector<double> resolved_percentiles;
    /// Resolved threshold after inheritance, populated by resolve().
    double resolved_threshold_pct = 0.0;
    /// Resolved sort order after inheritance, populated by resolve().
    std::string resolved_sort_by;
};

/// Top-level configuration for the comparison pipeline.
///
/// Constructed from CLI arguments via from_cli() or loaded from a
/// JSON file via from_json_file(). Call resolve() after construction
/// to propagate defaults down the node tree.
struct ComparisonConfig {
    /// Baseline trace file or directory path.
    std::string baseline;
    /// Variant trace file or directory path.
    std::string variant;
    /// Default settings inherited by all nodes.
    ComparisonDefaults defaults;
    /// Hierarchical comparison tree (top-level nodes).
    std::vector<ComparisonNode> nodes;

    /// Output file paths (CLI only, detect format by extension).
    std::vector<std::string> output_paths;
    /// Output format: "table" or "json".
    std::string format = "table";
    /// Disable ANSI color in table output.
    bool no_color = false;
    /// Number of parallel threads (0 = auto-detect).
    std::size_t executor_threads = 0;
    /// Checkpoint size for index building (0 = default).
    std::size_t checkpoint_size = 0;
    /// Directory for `.dftindex` stores.
    std::string index_dir;
    /// Force rebuild of existing indexes.
    bool force_rebuild = false;

    /// Parse a JSON config file. Returns nullopt and sets @p error on
    /// failure.
    static std::optional<ComparisonConfig> from_json_file(
        const std::string& path, std::string& error);

    /// Build config from CLI arguments (quick mode without JSON).
    /// Creates a single root node with the given query and group-by.
    static ComparisonConfig from_cli(const std::string& baseline,
                                     const std::string& variant,
                                     const std::string& query,
                                     const std::string& group_by_str);

    /// Resolve inheritance: compose queries and propagate defaults
    /// down the node tree. Must be called before using the config.
    void resolve();

   private:
    static bool parse_node(void* yyjson_val_ptr, ComparisonNode& node,
                           std::string& error);
    void resolve_node(ComparisonNode& node, const std::string& parent_query,
                      const std::vector<std::string>& parent_metrics,
                      const std::vector<double>& parent_percentiles,
                      double parent_threshold,
                      const std::string& parent_sort_by);
};

}  // namespace dftracer::utils::utilities::composites::dft::comparator

#endif
