#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_COMPARATOR_TREE_TABLE_FORMATTER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_COMPARATOR_TREE_TABLE_FORMATTER_H

#include <dftracer/utils/utilities/composites/dft/comparator/comparison_result.h>

#include <cstdio>
#include <string>

namespace dftracer::utils::utilities::composites::dft::comparator {

/// Options controlling the visual appearance of rendered output.
struct FormatterOptions {
    /// Enable ANSI color escape codes in table output.
    bool use_color = true;
    /// Use Unicode box-drawing characters for tree branches.
    bool use_unicode = true;
};

/// Dynamically computed column widths for aligned table output.
struct ColumnWidths {
    int left = 6;      ///< Left column (metric/node name).
    int baseline = 8;  ///< Baseline value column.
    int variant = 7;   ///< Variant value column.
    int delta = 5;     ///< Delta column.
    int change = 6;    ///< Percentage change column.
};

/// Renders ComparisonOutput as an ASCII tree table or JSON.
///
/// The tree table uses dynamic column alignment with UTF-8 display
/// width awareness. Nodes are rendered hierarchically with tree
/// branch characters, and metrics are shown as leaves under each
/// node.
class TreeTableFormatter {
   public:
    /// Construct a formatter with the given display options.
    explicit TreeTableFormatter(FormatterOptions options = {});

    /// Render the comparison output as a tree table to a FILE stream.
    void render(std::FILE* out, const ComparisonOutput& output) const;

    /// Render the comparison output as a JSON string.
    std::string render_json(const ComparisonOutput& output) const;

   private:
    FormatterOptions options_;

    const char* branch_mid() const;
    const char* branch_last() const;
    const char* branch_cont() const;
    const char* branch_none() const;

    const char* color_red() const;
    const char* color_green() const;
    const char* color_yellow() const;
    const char* color_bold() const;
    const char* color_dim() const;
    const char* color_reset() const;

    std::string format_value(double value, const std::string& metric) const;
    std::string format_delta(double delta, const std::string& metric) const;
    std::string format_pct(double pct) const;
    std::string format_significance(Significance sig) const;
    std::string format_val_str(const MetricComparison& mc, bool is_base) const;

    void measure_metrics_tree(const std::vector<MetricComparison>& metrics,
                              const std::string& prefix,
                              ColumnWidths& cw) const;
    void measure_node(const NodeResult& node, const std::string& prefix,
                      bool is_last, bool is_top_level, ColumnWidths& cw) const;

    void render_leaf(std::FILE* out, const MetricComparison& mc,
                     const std::string& leaf_prefix,
                     const std::string& leaf_name,
                     const ColumnWidths& cw) const;
    void render_metrics_tree(std::FILE* out,
                             const std::vector<MetricComparison>& metrics,
                             const std::string& prefix, bool is_last_section,
                             const ColumnWidths& cw) const;
    void render_node(std::FILE* out, const NodeResult& node,
                     const std::string& prefix, bool is_last, bool is_top_level,
                     const ColumnWidths& cw) const;
};

}  // namespace dftracer::utils::utilities::composites::dft::comparator

#endif
