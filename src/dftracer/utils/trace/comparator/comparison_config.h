// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_COMPARATOR_COMPARISON_CONFIG_H
#define DFTRACER_UTILS_TRACE_COMPARATOR_COMPARISON_CONFIG_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <simdjson.h>

#include <cstddef>

namespace dftracer::utils::trace::comparator {

/// Default settings inherited by all comparison nodes unless overridden.
struct ComparisonDefaults {
    /// Metric groups to compare (count, duration, size, transfer_size,
    /// bandwidth).
    dstd::Vector<dstd::String> metrics = {"count", "duration", "size",
                                          "transfer_size", "bandwidth"};
    /// Percentiles to compute from DDSketch (e.g. p50, p95, p99).
    dstd::Vector<double> percentiles = {0.50, 0.95, 0.99};
    /// Hide changes below this percentage in the output.
    double threshold_pct = 0.0;
    /// Time bucket width in milliseconds for aggregation windows.
    double time_interval_ms = 5000.0;
    /// Sort order for groups: "regression" sorts worst regressions first.
    dstd::String sort_by = "regression";
};

/// A node in the hierarchical comparison tree.
///
/// Each node carries a query that is AND'd with its parent's query,
/// and optional per-node overrides for metrics, percentiles, and
/// threshold. Children inherit from their parent unless they override.
struct ComparisonNode {
    /// Display name for this node (e.g. "POSIX I/O", "reads").
    dstd::String name;
    /// Additional query filter, AND'd with the parent's composed query.
    dstd::String query;
    /// Extra group-by keys beyond the default (cat, name).
    dstd::Vector<dstd::String> group_by;

    /// Per-node metric override (nullopt = inherit from parent).
    dstd::Optional<dstd::Vector<dstd::String>> metrics;
    /// Per-node percentile override (nullopt = inherit from parent).
    dstd::Optional<dstd::Vector<double>> percentiles;
    /// Per-node threshold override (nullopt = inherit from parent).
    dstd::Optional<double> threshold_pct;
    /// Per-node sort override (nullopt = inherit from parent).
    dstd::Optional<dstd::String> sort_by;

    /// Child nodes forming the comparison hierarchy.
    dstd::Vector<ComparisonNode> children;

    /// Full query including all ancestor queries, populated by
    /// resolve().
    dstd::String composed_query;
    /// Resolved metrics after inheritance, populated by resolve().
    dstd::Vector<dstd::String> resolved_metrics;
    /// Resolved percentiles after inheritance, populated by resolve().
    dstd::Vector<double> resolved_percentiles;
    /// Resolved threshold after inheritance, populated by resolve().
    double resolved_threshold_pct = 0.0;
    /// Resolved sort order after inheritance, populated by resolve().
    dstd::String resolved_sort_by;
};

/// Top-level configuration for the comparison pipeline.
///
/// Constructed from CLI arguments via from_cli() or loaded from a
/// JSON file via from_json_file(). Call resolve() after construction
/// to propagate defaults down the node tree.
struct ComparisonConfig {
    /// Baseline trace file or directory path.
    dstd::String baseline;
    /// Variant trace file or directory path.
    dstd::String variant;
    /// Default settings inherited by all nodes.
    ComparisonDefaults defaults;
    /// Hierarchical comparison tree (top-level nodes).
    dstd::Vector<ComparisonNode> nodes;

    /// Output file paths (CLI only, detect format by extension).
    dstd::Vector<dstd::String> output_paths;
    /// Output format: "table" or "json".
    dstd::String format = "table";
    /// Disable ANSI color in table output.
    bool no_color = false;
    /// Collapse nodes with only negligible changes to a single line.
    bool compact = false;
    /// Number of parallel threads (0 = auto-detect).
    std::size_t executor_threads = 0;
    /// Checkpoint size for index building (0 = default).
    std::size_t checkpoint_size = 0;
    /// Directory for baseline `.dftindex` store (empty = co-located).
    dstd::String baseline_index_dir;
    /// Directory for variant `.dftindex` store (empty = co-located).
    dstd::String variant_index_dir;
    /// Force rebuild of existing indexes.
    bool force_rebuild = false;

    /// Parse a JSON config file. Returns nullopt and sets @p error on
    /// failure.
    static dstd::Optional<ComparisonConfig> from_json_file(
        const dstd::String& path, dstd::String& error);

    /// Build config from CLI arguments (quick mode without JSON).
    /// Creates a single root node with the given query and group-by.
    static ComparisonConfig from_cli(const dstd::String& baseline,
                                     const dstd::String& variant,
                                     const dstd::String& query,
                                     const dstd::String& group_by_str);

    /// Build a preset comparison config. Returns nullopt for unknown presets.
    /// Supported presets: "dlio".
    static dstd::Optional<ComparisonConfig> from_preset(
        const dstd::String& preset, const dstd::String& baseline,
        const dstd::String& variant);

    /// Resolve inheritance: compose queries and propagate defaults
    /// down the node tree. Must be called before using the config.
    void resolve();

   private:
    static bool parse_node(simdjson::dom::element val, ComparisonNode& node,
                           dstd::String& error);
    void resolve_node(ComparisonNode& node, const dstd::String& parent_query,
                      const dstd::Vector<dstd::String>& parent_metrics,
                      const dstd::Vector<double>& parent_percentiles,
                      double parent_threshold,
                      const dstd::String& parent_sort_by);
};

}  // namespace dftracer::utils::trace::comparator

#endif
