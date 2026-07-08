#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/utilities/common/json/json_escape.h>
#include <dftracer/utils/utilities/composites/dft/comparator/tree_table_formatter.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::comparator {

using common::json::escape_json_string;

TreeTableFormatter::TreeTableFormatter(FormatterOptions options)
    : options_(options) {}

// ---------------------------------------------------------------------------
// Tree drawing characters
// ---------------------------------------------------------------------------

const char* TreeTableFormatter::branch_mid() const {
    // ├──
    return options_.use_unicode ? "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 "
                                : "+-- ";
}

const char* TreeTableFormatter::branch_last() const {
    // └──
    return options_.use_unicode ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "
                                : "`-- ";
}

const char* TreeTableFormatter::branch_cont() const {
    // │
    return options_.use_unicode ? "\xe2\x94\x82   " : "|   ";
}

const char* TreeTableFormatter::branch_none() const { return "    "; }

// ---------------------------------------------------------------------------
// ANSI color helpers
// ---------------------------------------------------------------------------

const char* TreeTableFormatter::color_red() const {
    return options_.use_color ? "\033[31m" : "";
}

const char* TreeTableFormatter::color_green() const {
    return options_.use_color ? "\033[32m" : "";
}

const char* TreeTableFormatter::color_yellow() const {
    return options_.use_color ? "\033[33m" : "";
}

const char* TreeTableFormatter::color_bold() const {
    return options_.use_color ? "\033[1m" : "";
}

const char* TreeTableFormatter::color_dim() const {
    return options_.use_color ? "\033[2m" : "";
}

const char* TreeTableFormatter::color_reset() const {
    return options_.use_color ? "\033[0m" : "";
}

// ---------------------------------------------------------------------------
// Value formatting helpers
// ---------------------------------------------------------------------------

namespace {

std::string fmt_with_commas(double v) {
    auto n = static_cast<long long>(v);
    std::string s = std::to_string(n);
    int insert_pos = static_cast<int>(s.size()) - 3;
    while (insert_pos > 0) {
        s.insert(static_cast<std::size_t>(insert_pos), ",");
        insert_pos -= 3;
    }
    return s;
}

std::string fmt_duration(double us) {
    char buf[32];
    if (us < 1.0) {
        std::snprintf(buf, sizeof(buf), "%.0f ns", us * 1000.0);
    } else if (us < 1000.0) {
        std::snprintf(buf, sizeof(buf), "%.1f us", us);
    } else if (us < 1000000.0) {
        std::snprintf(buf, sizeof(buf), "%.2f ms", us / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.3f s", us / 1000000.0);
    }
    return buf;
}

// Format a byte-scaled quantity with a binary (1024) ladder. `suffix` is
// appended to the unit ("" for sizes, "/s" for bandwidth).
std::string fmt_bytes_scaled(double value, const char* suffix) {
    char buf[32];
    constexpr double KB = 1024.0;
    constexpr double MB = 1024.0 * 1024.0;
    constexpr double GB = 1024.0 * 1024.0 * 1024.0;
    if (value < KB) {
        std::snprintf(buf, sizeof(buf), "%.0f B%s", value, suffix);
    } else if (value < MB) {
        std::snprintf(buf, sizeof(buf), "%.1f KB%s", value / KB, suffix);
    } else if (value < GB) {
        std::snprintf(buf, sizeof(buf), "%.2f MB%s", value / MB, suffix);
    } else {
        std::snprintf(buf, sizeof(buf), "%.3f GB%s", value / GB, suffix);
    }
    return buf;
}

std::string fmt_size(double bytes) { return fmt_bytes_scaled(bytes, ""); }

// True when every metric in the group is zero on both sides.
bool all_group_zero(const std::vector<const MetricComparison*>& group) {
    for (const auto* mc : group) {
        if (mc->baseline_value != 0.0 || mc->variant_value != 0.0) return false;
    }
    return true;
}

// A named bucket of metrics: standalone metrics keep their own name; metrics
// sharing a metric_group() prefix collapse under that group name. Groups that
// are all-zero on both sides are dropped. Item pointers alias `metrics`.
struct MetricGroup {
    std::string name;
    std::vector<const MetricComparison*> items;
};

std::vector<MetricGroup> group_metrics(
    const std::vector<MetricComparison>& metrics) {
    std::vector<MetricGroup> groups;
    std::unordered_map<std::string, std::size_t> group_idx;

    for (const auto& mc : metrics) {
        if (mc.baseline_value == 0.0 && mc.variant_value == 0.0) continue;
        std::string grp = metric_group(mc.metric_name);
        if (grp.empty()) {
            groups.push_back({mc.metric_name, {&mc}});
        } else {
            auto it = group_idx.find(grp);
            if (it == group_idx.end()) {
                group_idx[grp] = groups.size();
                groups.push_back({grp, {&mc}});
            } else {
                groups[it->second].items.push_back(&mc);
            }
        }
    }

    groups.erase(std::remove_if(groups.begin(), groups.end(),
                                [](const MetricGroup& g) {
                                    return all_group_zero(g.items);
                                }),
                 groups.end());
    return groups;
}

// True when any metric has a non-zero value on either side.
bool metrics_have_data(const std::vector<MetricComparison>& metrics) {
    for (const auto& mc : metrics) {
        if (mc.baseline_value != 0.0 || mc.variant_value != 0.0) return true;
    }
    return false;
}

// True when a node has any renderable content (recursively).
bool node_has_data(const NodeResult& node) {
    if (metrics_have_data(node.summary.metrics)) return true;
    if (!node.groups.empty()) return true;
    for (const auto& child : node.children) {
        if (node_has_data(child)) return true;
    }
    return false;
}

// True when every non-zero metric in the list has negligible significance.
bool metrics_all_negligible(const std::vector<MetricComparison>& metrics) {
    for (const auto& mc : metrics) {
        if ((mc.baseline_value != 0.0 || mc.variant_value != 0.0) &&
            mc.significance > Significance::NEGLIGIBLE) {
            return false;
        }
    }
    return true;
}

// True when a node has data but no metric anywhere in its subtree is
// significant. Returns false for no-data nodes (those are handled separately).
bool node_all_negligible(const NodeResult& node) {
    if (!node_has_data(node)) return false;
    if (!metrics_all_negligible(node.summary.metrics)) return false;
    for (const auto& g : node.groups) {
        if (!metrics_all_negligible(g.metrics)) return false;
    }
    for (const auto& child : node.children) {
        if (!node_all_negligible(child)) return false;
    }
    return true;
}

// Per-tree stats used for the summary preamble.
struct NodeStats {
    int total = 0;
    int no_data = 0;
    int regression = 0;   // subtrees with at least one MEDIUM+ regression
    int improvement = 0;  // subtrees with at least one MEDIUM+ improvement
};

static std::pair<bool, bool> gather_node_stats_impl(const NodeResult& node,
                                                    NodeStats& stats) {
    stats.total++;

    bool has_reg = false, has_imp = false;
    auto scan = [&](const std::vector<MetricComparison>& metrics) {
        for (const auto& mc : metrics) {
            if (mc.significance >= Significance::MEDIUM) {
                if (mc.is_regression)
                    has_reg = true;
                else
                    has_imp = true;
            }
        }
    };
    scan(node.summary.metrics);
    for (const auto& g : node.groups) scan(g.metrics);

    for (const auto& child : node.children) {
        auto [cr, ci] = gather_node_stats_impl(child, stats);
        has_reg |= cr;
        has_imp |= ci;
    }

    if (!node_has_data(node)) {
        stats.no_data++;
    } else {
        if (has_reg) stats.regression++;
        if (has_imp) stats.improvement++;
    }
    return {has_reg, has_imp};
}

static void gather_node_stats(const NodeResult& node, NodeStats& stats) {
    gather_node_stats_impl(node, stats);
}

struct RegressionEntry {
    std::string path;
    std::string metric;
    double pct_change = 0.0;
    double baseline = 0.0;
    double variant = 0.0;
    Significance sig = Significance::NEGLIGIBLE;
};

static void collect_regressions(const NodeResult& node,
                                const std::string& prefix,
                                std::vector<RegressionEntry>& out) {
    std::string here = prefix.empty() ? node.name : prefix + " > " + node.name;

    auto push_regressions = [&](const std::vector<MetricComparison>& metrics,
                                const std::string& path) {
        for (const auto& mc : metrics) {
            if (mc.significance >= Significance::MEDIUM && mc.is_regression &&
                (mc.baseline_value != 0.0 || mc.variant_value != 0.0)) {
                out.push_back({path, mc.metric_name, mc.pct_change,
                               mc.baseline_value, mc.variant_value,
                               mc.significance});
            }
        }
    };

    push_regressions(node.summary.metrics, here);
    for (const auto& g : node.groups) {
        push_regressions(g.metrics, here + " [" + g.label + "]");
    }
    for (const auto& child : node.children) {
        collect_regressions(child, here, out);
    }
}

std::string fmt_bandwidth(double bps) { return fmt_bytes_scaled(bps, "/s"); }

std::string fmt_generic(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// Display width of a UTF-8 string (characters, not bytes).
// Counts one display column per Unicode code point by skipping
// UTF-8 continuation bytes (10xxxxxx).
int display_width(const std::string& s) {
    int w = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++w;
    }
    return w;
}

// Right-pad string to target display width.
void rpad(std::string& s, int target) {
    int pad = target - display_width(s);
    if (pad > 0) s.append(static_cast<std::size_t>(pad), ' ');
}

// Left-pad (right-align) string to target display width.
void lpad(std::string& s, int target) {
    int pad = target - display_width(s);
    if (pad > 0) s.insert(0, static_cast<std::size_t>(pad), ' ');
}

}  // namespace

std::string TreeTableFormatter::format_value(double v,
                                             const std::string& m) const {
    if (v == 0.0) return "0";
    std::string grp = metric_group(m);
    if (grp == "dur") return fmt_duration(v);
    if (grp == "size") return fmt_size(v);
    if (grp == "time") return fmt_duration(v);
    if (m == "transfer_size" || m == "total_bytes") return fmt_size(v);
    if (m == "bandwidth") return fmt_bandwidth(v);
    if (m == "count" || m == "unique_files" || m == "proc_files" ||
        m == "processes" || m == "threads")
        return fmt_with_commas(v);
    return fmt_generic(v);
}

std::string TreeTableFormatter::format_delta(double d,
                                             const std::string& m) const {
    std::string base;
    std::string grp = metric_group(m);
    if (grp == "dur" || grp == "time") {
        base = fmt_duration(std::abs(d));
    } else if (grp == "size") {
        base = fmt_size(std::abs(d));
    } else if (m == "transfer_size" || m == "total_bytes") {
        base = fmt_size(std::abs(d));
    } else if (m == "bandwidth") {
        base = fmt_bandwidth(std::abs(d));
    } else if (m == "count" || m == "unique_files" || m == "proc_files" ||
               m == "processes" || m == "threads") {
        base = fmt_with_commas(std::abs(d));
    } else {
        base = fmt_generic(std::abs(d));
    }
    return (d >= 0.0 ? "+" : "-") + base;
}

std::string TreeTableFormatter::format_pct(double pct) const {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%+.1f%%", pct);
    return buf;
}

std::string TreeTableFormatter::format_significance(Significance sig) const {
    switch (sig) {
        case Significance::NEGLIGIBLE:
            return std::string(color_dim()) + "~" + color_reset();
        case Significance::SMALL:
            return "*";
        case Significance::MEDIUM:
            return "**";
        case Significance::LARGE:
            return "***";
    }
    return "";
}

// Builds the full value string with optional (±stdev).
std::string TreeTableFormatter::format_val_str(const MetricComparison& mc,
                                               bool is_base) const {
    double val = is_base ? mc.baseline_value : mc.variant_value;
    double sd = is_base ? mc.baseline_stdev : mc.variant_stdev;
    std::string s = format_value(val, mc.metric_name);
    if (sd > 0.0) {
        s += " (\xc2\xb1" + format_value(sd, mc.metric_name) + ")";
    }
    return s;
}

// ---------------------------------------------------------------------------
// render_metrics_tree  (and its measure counterpart)
//
// Renders metrics as a sub-tree:
//   {prefix}├── count  | ...
//   {prefix}├── dur
//   {prefix}│   ├── mean  | ...
//   {prefix}│   └── p50   | ...
//   {prefix}└── size
//       {prefix}    └── mean | ...
//
// `prefix`          — continuation chars already printed for the parent level
// `is_last_section` — whether this metrics block is the last child of its
//                     parent (controls the branch char for the first item)
// ---------------------------------------------------------------------------

void TreeTableFormatter::measure_metrics_tree(
    const std::vector<MetricComparison>& metrics, const std::string& prefix,
    ColumnWidths& cw) const {
    // branch_mid/last are 4 display columns each (unicode or ASCII).
    const int BRANCH_W = 4;

    auto groups = group_metrics(metrics);

    const int prefix_dw = display_width(prefix);

    for (const auto& g : groups) {
        bool is_standalone = g.items.size() == 1 &&
                             metric_group(g.items[0]->metric_name).empty();
        if (is_standalone) {
            int w = prefix_dw + BRANCH_W + static_cast<int>(g.name.size());
            if (w > cw.left) cw.left = w;

            // Measure value columns for this leaf.
            const MetricComparison& mc = *g.items[0];
            std::string bs = format_val_str(mc, true);
            std::string vs = format_val_str(mc, false);
            std::string ds = format_delta(mc.delta, mc.metric_name);
            std::string ps = format_pct(mc.pct_change);
            int bw = display_width(bs);
            int vw = display_width(vs);
            int dw = display_width(ds);
            int pw = display_width(ps);
            if (bw > cw.baseline) cw.baseline = bw;
            if (vw > cw.variant) cw.variant = vw;
            if (dw > cw.delta) cw.delta = dw;
            if (pw > cw.change) cw.change = pw;
        } else {
            // Sub-node header.
            int w = prefix_dw + BRANCH_W + static_cast<int>(g.name.size());
            if (w > cw.left) cw.left = w;

            // Children: prefix + branch + cont + leaf_name.
            for (const auto* mc : g.items) {
                std::string leaf = metric_leaf(mc->metric_name);
                int lw = prefix_dw + BRANCH_W + BRANCH_W +
                         static_cast<int>(leaf.size());
                if (lw > cw.left) cw.left = lw;

                std::string bs = format_val_str(*mc, true);
                std::string vs = format_val_str(*mc, false);
                std::string ds = format_delta(mc->delta, mc->metric_name);
                std::string ps = format_pct(mc->pct_change);
                int bw = display_width(bs);
                int vw = display_width(vs);
                int dw = display_width(ds);
                int pw = display_width(ps);
                if (bw > cw.baseline) cw.baseline = bw;
                if (vw > cw.variant) cw.variant = vw;
                if (dw > cw.delta) cw.delta = dw;
                if (pw > cw.change) cw.change = pw;
            }
        }
    }
}

void TreeTableFormatter::render_leaf(std::FILE* out, const MetricComparison& mc,
                                     const std::string& leaf_prefix,
                                     const std::string& leaf_name,
                                     const ColumnWidths& cw) const {
    const char* col = "";
    if (mc.significance >= Significance::MEDIUM) {
        col = mc.is_regression ? color_red() : color_green();
    } else if (mc.significance == Significance::SMALL) {
        col = color_yellow();
    } else {
        col = color_dim();
    }

    std::string base_s = format_val_str(mc, true);
    std::string var_s = format_val_str(mc, false);
    std::string delta_s = format_delta(mc.delta, mc.metric_name);
    std::string pct_s = format_pct(mc.pct_change);
    std::string sig_s = format_significance(mc.significance);

    std::string left = leaf_prefix + leaf_name;
    rpad(left, cw.left);
    lpad(base_s, cw.baseline);
    lpad(var_s, cw.variant);
    lpad(delta_s, cw.delta);
    lpad(pct_s, cw.change);

    std::fprintf(out, "%s | %s | %s | %s%s | %s | %s%s\n", left.c_str(),
                 base_s.c_str(), var_s.c_str(), col, delta_s.c_str(),
                 pct_s.c_str(), sig_s.c_str(), color_reset());
}

void TreeTableFormatter::render_metrics_tree(
    std::FILE* out, const std::vector<MetricComparison>& metrics,
    const std::string& prefix, bool /*is_last_section*/,
    const ColumnWidths& cw) const {
    auto groups = group_metrics(metrics);

    if (groups.empty()) return;

    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        bool is_last_group = (gi + 1 == groups.size());
        const char* br = is_last_group ? branch_last() : branch_mid();
        const auto& g = groups[gi];

        bool is_standalone = g.items.size() == 1 &&
                             metric_group(g.items[0]->metric_name).empty();
        if (is_standalone) {
            render_leaf(out, *g.items[0], prefix + br, g.name, cw);
        } else {
            std::fprintf(out, "%s%s%s\n", prefix.c_str(), br, g.name.c_str());

            std::string cont =
                prefix + (is_last_group ? branch_none() : branch_cont());
            for (std::size_t i = 0; i < g.items.size(); ++i) {
                bool leaf_last = (i + 1 == g.items.size());
                const char* lbr = leaf_last ? branch_last() : branch_mid();
                render_leaf(out, *g.items[i], cont + lbr,
                            metric_leaf(g.items[i]->metric_name), cw);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// measure_node / render_node
// ---------------------------------------------------------------------------

void TreeTableFormatter::measure_node(const NodeResult& node,
                                      const std::string& prefix, bool is_last,
                                      bool is_top_level,
                                      ColumnWidths& cw) const {
    if (!node_has_data(node)) return;
    if (options_.compact && node_all_negligible(node)) return;

    // Continuation prefix inside this node.
    std::string cont;
    if (is_top_level) {
        cont = std::string(is_last ? branch_none() : branch_cont());
    } else {
        cont = prefix + (is_last ? branch_none() : branch_cont());
    }

    const int BRANCH_W = 4;
    const int cont_dw = display_width(cont);

    bool has_summary_data = metrics_have_data(node.summary.metrics);
    bool has_groups = !node.groups.empty();
    bool has_children = !node.children.empty();

    // SUMMARY sub-node - only measure when it has data.
    if (has_summary_data) {
        int w = cont_dw + BRANCH_W +
                static_cast<int>(std::string("SUMMARY").size());
        if (w > cw.left) cw.left = w;
        bool summary_last = !has_groups && !has_children;
        std::string summary_cont =
            cont + (summary_last ? branch_none() : branch_cont());
        measure_metrics_tree(node.summary.metrics, summary_cont, cw);
    }

    // Per-operation groups.
    for (std::size_t i = 0; i < node.groups.size(); ++i) {
        bool g_last = (i + 1 == node.groups.size()) && !has_children;
        int w =
            cont_dw + BRANCH_W + static_cast<int>(node.groups[i].label.size());
        if (w > cw.left) cw.left = w;
        std::string g_cont = cont + (g_last ? branch_none() : branch_cont());
        measure_metrics_tree(node.groups[i].metrics, g_cont, cw);
    }

    // Recurse into children.
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        bool child_last = (i + 1 == node.children.size());
        measure_node(node.children[i], cont, child_last, false, cw);
    }
}

void TreeTableFormatter::render_node(std::FILE* out, const NodeResult& node,
                                     const std::string& prefix, bool is_last,
                                     bool is_top_level,
                                     const ColumnWidths& cw) const {
    const char* branch = is_last ? branch_last() : branch_mid();
    std::string cont;

    bool has_data = node_has_data(node);

    bool negligible = has_data && options_.compact && node_all_negligible(node);

    // Determine the inline annotation to append to the node name.
    const char* annotation = "";
    const char* ann_open = "";
    const char* ann_close = "";
    if (!has_data) {
        annotation = "(no data)";
        ann_open = color_dim();
        ann_close = color_reset();
    } else if (negligible) {
        annotation = "(no change)";
        ann_open = color_dim();
        ann_close = color_reset();
    }

    if (is_top_level) {
        if (*annotation) {
            std::fprintf(out, "%s%s%s%s %s%s%s\n", branch, color_bold(),
                         node.name.c_str(), color_reset(), ann_open, annotation,
                         ann_close);
        } else {
            std::fprintf(out, "%s%s%s%s\n", branch, color_bold(),
                         node.name.c_str(), color_reset());
        }
        cont = std::string(is_last ? branch_none() : branch_cont());
    } else {
        if (*annotation) {
            std::fprintf(out, "%s%s%s%s%s %s%s%s\n", prefix.c_str(), branch,
                         color_bold(), node.name.c_str(), color_reset(),
                         ann_open, annotation, ann_close);
        } else {
            std::fprintf(out, "%s%s%s%s%s\n", prefix.c_str(), branch,
                         color_bold(), node.name.c_str(), color_reset());
        }
        cont = prefix + (is_last ? branch_none() : branch_cont());
    }

    if (!has_data || negligible) return;

    bool has_summary_data = metrics_have_data(node.summary.metrics);
    bool has_groups = !node.groups.empty();
    bool has_children = !node.children.empty();

    // SUMMARY sub-node - only render when it has data.
    if (has_summary_data) {
        bool summary_last = !has_groups && !has_children;
        const char* sbr = summary_last ? branch_last() : branch_mid();
        std::fprintf(out, "%s%s%sSUMMARY%s\n", cont.c_str(), sbr, color_bold(),
                     color_reset());
        std::string summary_cont =
            cont + (summary_last ? branch_none() : branch_cont());
        render_metrics_tree(out, node.summary.metrics, summary_cont,
                            summary_last, cw);
    }

    // Per-operation groups.
    for (std::size_t i = 0; i < node.groups.size(); ++i) {
        const auto& g = node.groups[i];
        bool g_last = (i + 1 == node.groups.size()) && !has_children;
        const char* gbr = g_last ? branch_last() : branch_mid();
        std::fprintf(out, "%s%s%s\n", cont.c_str(), gbr, g.label.c_str());
        std::string g_cont = cont + (g_last ? branch_none() : branch_cont());
        render_metrics_tree(out, g.metrics, g_cont, g_last, cw);
    }

    // Recurse into children.
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        bool child_last = (i + 1 == node.children.size());
        render_node(out, node.children[i], cont, child_last, false, cw);
    }
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------

void TreeTableFormatter::render(std::FILE* out,
                                const ComparisonOutput& output) const {
    std::fprintf(out, "Comparison:\n");
    std::fprintf(out, "  baseline: %s\n", output.baseline_path.c_str());
    std::fprintf(out, "  variant:  %s\n", output.variant_path.c_str());
    std::fprintf(out, "\n");

    // Summary preamble: one line with node-level counts.
    {
        NodeStats stats;
        for (const auto& n : output.nodes) {
            gather_node_stats(n, stats);
        }
        std::fprintf(out, "  %s%d node%s%s", color_bold(), stats.total,
                     stats.total == 1 ? "" : "s", color_reset());
        if (stats.regression > 0) {
            std::fprintf(out, "  %s%d regression%s%s", color_red(),
                         stats.regression, stats.regression == 1 ? "" : "s",
                         color_reset());
        }
        if (stats.improvement > 0) {
            std::fprintf(out, "  %s%d improvement%s%s", color_green(),
                         stats.improvement, stats.improvement == 1 ? "" : "s",
                         color_reset());
        }
        if (stats.no_data > 0) {
            std::fprintf(out, "  %s%d no data%s", color_dim(), stats.no_data,
                         color_reset());
        }
        std::fprintf(out, "\n\n");
    }

    // Pre-pass: compute all column widths.
    ColumnWidths cw;
    for (std::size_t i = 0; i < output.nodes.size(); ++i) {
        bool is_last = (i + 1 == output.nodes.size());
        measure_node(output.nodes[i], "", is_last, true, cw);
    }
    cw.left += 2;  // breathing room

    // Column header.
    {
        std::string hdr = "metric";
        rpad(hdr, cw.left);
        std::string bh = "baseline";
        lpad(bh, cw.baseline);
        std::string vh = "variant";
        lpad(vh, cw.variant);
        std::string dh = "delta";
        lpad(dh, cw.delta);
        std::string ch = "change";
        lpad(ch, cw.change);
        std::fprintf(out, "%s | %s | %s | %s | %s | %s\n", hdr.c_str(),
                     bh.c_str(), vh.c_str(), dh.c_str(), ch.c_str(), "sig");

        std::string sep(static_cast<std::size_t>(cw.left), '-');
        std::fprintf(
            out, "%s-+-%s-+-%s-+-%s-+-%s-+-%s\n", sep.c_str(),
            std::string(static_cast<std::size_t>(cw.baseline), '-').c_str(),
            std::string(static_cast<std::size_t>(cw.variant), '-').c_str(),
            std::string(static_cast<std::size_t>(cw.delta), '-').c_str(),
            std::string(static_cast<std::size_t>(cw.change), '-').c_str(),
            "---");
    }

    for (std::size_t i = 0; i < output.nodes.size(); ++i) {
        if (i > 0) std::fprintf(out, "\n");
        bool is_last = (i + 1 == output.nodes.size());
        render_node(out, output.nodes[i], "", is_last, true, cw);
    }

    // Top regressions footer.
    if (options_.top_regressions > 0) {
        std::vector<RegressionEntry> regs;
        for (const auto& n : output.nodes) {
            collect_regressions(n, "", regs);
        }
        std::sort(regs.begin(), regs.end(),
                  [](const RegressionEntry& a, const RegressionEntry& b) {
                      return a.pct_change > b.pct_change;
                  });
        if (!regs.empty()) {
            int show = std::min(options_.top_regressions,
                                static_cast<int>(regs.size()));
            std::fprintf(out, "\n%sTop %d regression%s:%s\n", color_bold(),
                         show, show == 1 ? "" : "s", color_reset());
            for (int i = 0; i < show; ++i) {
                const auto& r = regs[static_cast<std::size_t>(i)];
                const char* sig = r.sig == Significance::LARGE ? "***" : "**";
                std::fprintf(out, "  %s%s%s  %s  %s%+.1f%%%s  %s\n",
                             color_red(), r.path.c_str(), color_reset(),
                             r.metric.c_str(), color_red(), r.pct_change,
                             color_reset(), sig);
            }
        }
    }

    std::fprintf(out, "\n");
    std::fprintf(out, "Completed in %.1f ms\n", output.execution_time_ms);
}

// ---------------------------------------------------------------------------
// render_json helpers
// ---------------------------------------------------------------------------

namespace {

std::string double_to_json(double v) {
    if (!std::isfinite(v)) return "0";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.15g", v);
    return buf;
}

void build_metric_json(std::ostringstream& out, const MetricComparison& mc) {
    auto safe = [](double v) { return std::isfinite(v) ? v : 0.0; };
    out << "{";
    out << "\"name\":\"" << escape_json_string(mc.metric_name) << "\",";
    out << "\"baseline\":" << double_to_json(safe(mc.baseline_value)) << ",";
    out << "\"variant\":" << double_to_json(safe(mc.variant_value)) << ",";
    out << "\"delta\":" << double_to_json(safe(mc.delta)) << ",";
    out << "\"pct_change\":" << double_to_json(safe(mc.pct_change)) << ",";
    out << "\"cohens_d\":" << double_to_json(safe(mc.cohens_d)) << ",";
    out << "\"significance\":\"" << significance_to_string(mc.significance)
        << "\",";
    out << "\"is_regression\":" << (mc.is_regression ? "true" : "false");
    out << "}";
}

void build_metrics_arr(std::ostringstream& out,
                       const std::vector<MetricComparison>& ms) {
    out << "[";
    for (std::size_t i = 0; i < ms.size(); ++i) {
        if (i > 0) out << ",";
        build_metric_json(out, ms[i]);
    }
    out << "]";
}

void build_group_json(std::ostringstream& out, const GroupComparison& g) {
    out << "{";
    out << "\"label\":\"" << escape_json_string(g.label) << "\",";
    out << "\"metrics\":";
    build_metrics_arr(out, g.metrics);
    out << "}";
}

void build_node_json(std::ostringstream& out, const NodeResult& node);

void build_node_json(std::ostringstream& out, const NodeResult& node) {
    out << "{";
    out << "\"name\":\"" << escape_json_string(node.name) << "\",";
    out << "\"query\":\"" << escape_json_string(node.composed_query) << "\",";

    // summary
    out << "\"summary\":{\"metrics\":";
    build_metrics_arr(out, node.summary.metrics);
    out << "},";

    // groups
    out << "\"groups\":[";
    for (std::size_t i = 0; i < node.groups.size(); ++i) {
        if (i > 0) out << ",";
        build_group_json(out, node.groups[i]);
    }
    out << "],";

    // children
    out << "\"children\":[";
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        if (i > 0) out << ",";
        build_node_json(out, node.children[i]);
    }
    out << "]";

    out << "}";
}

void build_meta_json(std::ostringstream& out, const TraceMetadata& m) {
    out << "{";
    out << "\"files\":" << m.file_count << ",";
    out << "\"processes\":" << m.process_count << ",";
    out << "\"threads\":" << m.thread_count << ",";
    out << "\"total_bytes\":" << double_to_json(m.total_bytes) << ",";
    out << "\"total_io_time_us\":" << double_to_json(m.total_io_time_us) << ",";
    out << "\"makespan_us\":" << double_to_json(m.makespan_us);
    out << "}";
}

}  // namespace

// ---------------------------------------------------------------------------
// render_json
// ---------------------------------------------------------------------------

std::string TreeTableFormatter::render_json(
    const ComparisonOutput& output) const {
    std::ostringstream out;

    out << "{";
    out << "\"baseline\":\"" << escape_json_string(output.baseline_path)
        << "\",";
    out << "\"variant\":\"" << escape_json_string(output.variant_path) << "\",";
    out << "\"baseline_meta\":";
    build_meta_json(out, output.baseline_meta);
    out << ",";
    out << "\"variant_meta\":";
    build_meta_json(out, output.variant_meta);
    out << ",";
    out << "\"execution_time_ms\":" << double_to_json(output.execution_time_ms)
        << ",";

    out << "\"nodes\":[";
    for (std::size_t i = 0; i < output.nodes.size(); ++i) {
        if (i > 0) out << ",";
        build_node_json(out, output.nodes[i]);
    }
    out << "]";

    out << "}";

    return out.str();
}

}  // namespace dftracer::utils::utilities::composites::dft::comparator
