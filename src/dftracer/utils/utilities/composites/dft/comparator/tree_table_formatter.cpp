#include <dftracer/utils/utilities/composites/dft/comparator/tree_table_formatter.h>
#include <yyjson.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::comparator {

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

std::string fmt_size(double bytes) {
    char buf[32];
    constexpr double KB = 1024.0;
    constexpr double MB = 1024.0 * 1024.0;
    constexpr double GB = 1024.0 * 1024.0 * 1024.0;
    if (bytes < KB) {
        std::snprintf(buf, sizeof(buf), "%.0f B", bytes);
    } else if (bytes < MB) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / KB);
    } else if (bytes < GB) {
        std::snprintf(buf, sizeof(buf), "%.2f MB", bytes / MB);
    } else {
        std::snprintf(buf, sizeof(buf), "%.3f GB", bytes / GB);
    }
    return buf;
}

// Extract the group prefix: "dur_mean" -> "dur", "size_p50" -> "size".
// Returns "" for standalone metrics (no prefix, or known atomic names).
std::string metric_group(const std::string& name) {
    // Atomic metric names that must not be split on '_'.
    if (name == "transfer_size" || name == "bandwidth" || name == "total_bytes")
        return "";
    auto pos = name.find('_');
    if (pos == std::string::npos) return "";
    return name.substr(0, pos);
}

// Strip the group prefix: "dur_mean" -> "mean", "size_p50" -> "p50".
// Returns the full name when there is no prefix (or atomic).
std::string strip_prefix(const std::string& name) {
    if (name == "transfer_size" || name == "bandwidth" || name == "total_bytes")
        return name;
    auto pos = name.find('_');
    if (pos == std::string::npos) return name;
    return name.substr(pos + 1);
}

// True when every metric in the group is zero on both sides.
bool all_group_zero(const std::vector<const MetricComparison*>& group) {
    for (const auto* mc : group) {
        if (mc->baseline_value != 0.0 || mc->variant_value != 0.0) return false;
    }
    return true;
}

std::string fmt_bandwidth(double bps) {
    char buf[32];
    constexpr double KB = 1024.0;
    constexpr double MB = 1024.0 * 1024.0;
    constexpr double GB = 1024.0 * 1024.0 * 1024.0;
    if (bps < KB) {
        std::snprintf(buf, sizeof(buf), "%.0f B/s", bps);
    } else if (bps < MB) {
        std::snprintf(buf, sizeof(buf), "%.1f KB/s", bps / KB);
    } else if (bps < GB) {
        std::snprintf(buf, sizeof(buf), "%.2f MB/s", bps / MB);
    } else {
        std::snprintf(buf, sizeof(buf), "%.3f GB/s", bps / GB);
    }
    return buf;
}

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
    if (m == "count" || m == "files" || m == "processes" || m == "threads")
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
    } else if (m == "count" || m == "files" || m == "processes" ||
               m == "threads") {
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

    struct MetricGroup {
        std::string name;
        std::vector<const MetricComparison*> items;
    };
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
                std::string leaf = strip_prefix(mc->metric_name);
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
    struct MetricGroup {
        std::string name;
        std::vector<const MetricComparison*> items;
    };
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
                            strip_prefix(g.items[i]->metric_name), cw);
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
    // Continuation prefix inside this node.
    std::string cont;
    if (is_top_level) {
        cont = std::string(is_last ? branch_none() : branch_cont());
    } else {
        cont = prefix + (is_last ? branch_none() : branch_cont());
    }

    const int BRANCH_W = 4;
    const int cont_dw = display_width(cont);

    // SUMMARY sub-node header: cont + branch(4) + "SUMMARY"(7)
    {
        int w = cont_dw + BRANCH_W +
                static_cast<int>(std::string("SUMMARY").size());
        if (w > cw.left) cw.left = w;
    }

    // Metrics under SUMMARY.
    bool has_groups = !node.groups.empty();
    bool has_children = !node.children.empty();
    bool summary_last = !has_groups && !has_children;
    std::string summary_cont =
        cont + (summary_last ? branch_none() : branch_cont());
    measure_metrics_tree(node.summary.metrics, summary_cont, cw);

    // Per-operation groups.
    for (std::size_t i = 0; i < node.groups.size(); ++i) {
        bool g_last = (i + 1 == node.groups.size()) && !has_children;
        // Group header width.
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

    if (is_top_level) {
        std::fprintf(out, "%s%s%s%s\n", branch, color_bold(), node.name.c_str(),
                     color_reset());
        cont = std::string(is_last ? branch_none() : branch_cont());
    } else {
        std::fprintf(out, "%s%s%s%s%s\n", prefix.c_str(), branch, color_bold(),
                     node.name.c_str(), color_reset());
        cont = prefix + (is_last ? branch_none() : branch_cont());
    }

    bool has_groups = !node.groups.empty();
    bool has_children = !node.children.empty();

    // SUMMARY sub-node.
    {
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

    std::fprintf(out, "\n");
    std::fprintf(out, "Completed in %.1f ms\n", output.execution_time_ms);
}

// ---------------------------------------------------------------------------
// render_json helpers
// ---------------------------------------------------------------------------

namespace {

const char* sig_str(Significance s) {
    switch (s) {
        case Significance::NEGLIGIBLE:
            return "NEGLIGIBLE";
        case Significance::SMALL:
            return "SMALL";
        case Significance::MEDIUM:
            return "MEDIUM";
        case Significance::LARGE:
            return "LARGE";
    }
    return "NEGLIGIBLE";
}

yyjson_mut_val* build_metric_json(yyjson_mut_doc* doc,
                                  const MetricComparison& mc) {
    yyjson_mut_val* obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, obj, "name", mc.metric_name.c_str());
    yyjson_mut_obj_add_real(doc, obj, "baseline", mc.baseline_value);
    yyjson_mut_obj_add_real(doc, obj, "variant", mc.variant_value);
    yyjson_mut_obj_add_real(doc, obj, "delta", mc.delta);
    yyjson_mut_obj_add_real(doc, obj, "pct_change", mc.pct_change);
    yyjson_mut_obj_add_real(doc, obj, "cohens_d", mc.cohens_d);
    yyjson_mut_obj_add_str(doc, obj, "significance", sig_str(mc.significance));
    yyjson_mut_obj_add_bool(doc, obj, "is_regression", mc.is_regression);
    return obj;
}

yyjson_mut_val* build_metrics_arr(yyjson_mut_doc* doc,
                                  const std::vector<MetricComparison>& ms) {
    yyjson_mut_val* arr = yyjson_mut_arr(doc);
    for (const auto& mc : ms) {
        yyjson_mut_arr_append(arr, build_metric_json(doc, mc));
    }
    return arr;
}

yyjson_mut_val* build_group_json(yyjson_mut_doc* doc,
                                 const GroupComparison& g) {
    yyjson_mut_val* obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, obj, "label", g.label.c_str());
    yyjson_mut_obj_add_val(doc, obj, "metrics",
                           build_metrics_arr(doc, g.metrics));
    return obj;
}

yyjson_mut_val* build_node_json(yyjson_mut_doc* doc, const NodeResult& node);

yyjson_mut_val* build_node_json(yyjson_mut_doc* doc, const NodeResult& node) {
    yyjson_mut_val* obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, obj, "name", node.name.c_str());
    yyjson_mut_obj_add_str(doc, obj, "query", node.composed_query.c_str());

    // summary
    yyjson_mut_val* summary = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, summary, "metrics",
                           build_metrics_arr(doc, node.summary.metrics));
    yyjson_mut_obj_add_val(doc, obj, "summary", summary);

    // groups
    yyjson_mut_val* groups_arr = yyjson_mut_arr(doc);
    for (const auto& g : node.groups) {
        yyjson_mut_arr_append(groups_arr, build_group_json(doc, g));
    }
    yyjson_mut_obj_add_val(doc, obj, "groups", groups_arr);

    // children
    yyjson_mut_val* children_arr = yyjson_mut_arr(doc);
    for (const auto& child : node.children) {
        yyjson_mut_arr_append(children_arr, build_node_json(doc, child));
    }
    yyjson_mut_obj_add_val(doc, obj, "children", children_arr);

    return obj;
}

yyjson_mut_val* build_meta_json(yyjson_mut_doc* doc, const TraceMetadata& m) {
    yyjson_mut_val* obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, obj, "files",
                           static_cast<int64_t>(m.file_count));
    yyjson_mut_obj_add_int(doc, obj, "processes",
                           static_cast<int64_t>(m.process_count));
    yyjson_mut_obj_add_int(doc, obj, "threads",
                           static_cast<int64_t>(m.thread_count));
    yyjson_mut_obj_add_real(doc, obj, "total_bytes", m.total_bytes);
    yyjson_mut_obj_add_real(doc, obj, "total_io_time_us", m.total_io_time_us);
    yyjson_mut_obj_add_real(doc, obj, "makespan_us", m.makespan_us);
    return obj;
}

}  // namespace

// ---------------------------------------------------------------------------
// render_json
// ---------------------------------------------------------------------------

std::string TreeTableFormatter::render_json(
    const ComparisonOutput& output) const {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "baseline", output.baseline_path.c_str());
    yyjson_mut_obj_add_str(doc, root, "variant", output.variant_path.c_str());
    yyjson_mut_obj_add_val(doc, root, "baseline_meta",
                           build_meta_json(doc, output.baseline_meta));
    yyjson_mut_obj_add_val(doc, root, "variant_meta",
                           build_meta_json(doc, output.variant_meta));
    yyjson_mut_obj_add_real(doc, root, "execution_time_ms",
                            output.execution_time_ms);

    yyjson_mut_val* nodes_arr = yyjson_mut_arr(doc);
    for (const auto& node : output.nodes) {
        yyjson_mut_arr_append(nodes_arr, build_node_json(doc, node));
    }
    yyjson_mut_obj_add_val(doc, root, "nodes", nodes_arr);

    char* json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, nullptr);
    std::string result(json);
    free(json);  // NOLINT(cppcoreguidelines-no-malloc)
    yyjson_mut_doc_free(doc);

    return result;
}

}  // namespace dftracer::utils::utilities::composites::dft::comparator
