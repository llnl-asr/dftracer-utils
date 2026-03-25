#include <dftracer/utils/utilities/composites/dft/comparator/comparison_config.h>
#include <yyjson.h>

#include <sstream>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::comparator {

namespace {

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> result;
    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        // Trim leading/trailing whitespace
        auto start = token.find_first_not_of(" \t");
        auto end = token.find_last_not_of(" \t");
        if (start != std::string::npos) {
            result.push_back(token.substr(start, end - start + 1));
        }
    }
    return result;
}

}  // namespace

// static
bool ComparisonConfig::parse_node(void* yyjson_val_ptr, ComparisonNode& node,
                                  std::string& error) {
    auto* val = static_cast<yyjson_val*>(yyjson_val_ptr);
    if (!val || !yyjson_is_obj(val)) {
        error = "node must be a JSON object";
        return false;
    }

    yyjson_val* name_val = yyjson_obj_get(val, "name");
    if (!name_val || !yyjson_is_str(name_val)) {
        error = "node missing required string field 'name'";
        return false;
    }
    node.name = yyjson_get_str(name_val);

    yyjson_val* query_val = yyjson_obj_get(val, "query");
    if (query_val && yyjson_is_str(query_val)) {
        node.query = yyjson_get_str(query_val);
    }

    yyjson_val* gb_val = yyjson_obj_get(val, "group_by");
    if (gb_val && yyjson_is_arr(gb_val)) {
        std::size_t idx, max;
        yyjson_val* elem;
        yyjson_arr_foreach(gb_val, idx, max, elem) {
            if (yyjson_is_str(elem)) {
                node.group_by.push_back(yyjson_get_str(elem));
            }
        }
    }

    yyjson_val* metrics_val = yyjson_obj_get(val, "metrics");
    if (metrics_val && yyjson_is_arr(metrics_val)) {
        std::vector<std::string> metrics;
        std::size_t idx, max;
        yyjson_val* elem;
        yyjson_arr_foreach(metrics_val, idx, max, elem) {
            if (yyjson_is_str(elem)) {
                metrics.push_back(yyjson_get_str(elem));
            }
        }
        node.metrics = std::move(metrics);
    }

    yyjson_val* pct_val = yyjson_obj_get(val, "percentiles");
    if (pct_val && yyjson_is_arr(pct_val)) {
        std::vector<double> percentiles;
        std::size_t idx, max;
        yyjson_val* elem;
        yyjson_arr_foreach(pct_val, idx, max, elem) {
            if (yyjson_is_num(elem)) {
                percentiles.push_back(yyjson_get_num(elem));
            }
        }
        node.percentiles = std::move(percentiles);
    }

    yyjson_val* thr_val = yyjson_obj_get(val, "threshold_pct");
    if (thr_val && yyjson_is_num(thr_val)) {
        node.threshold_pct = yyjson_get_num(thr_val);
    }

    yyjson_val* sort_val = yyjson_obj_get(val, "sort_by");
    if (sort_val && yyjson_is_str(sort_val)) {
        node.sort_by = yyjson_get_str(sort_val);
    }

    yyjson_val* children_val = yyjson_obj_get(val, "children");
    if (children_val && yyjson_is_arr(children_val)) {
        std::size_t idx, max;
        yyjson_val* child_elem;
        yyjson_arr_foreach(children_val, idx, max, child_elem) {
            ComparisonNode child;
            if (!parse_node(child_elem, child, error)) return false;
            node.children.push_back(std::move(child));
        }
    }

    return true;
}

// static
std::optional<ComparisonConfig> ComparisonConfig::from_json_file(
    const std::string& path, std::string& error) {
    yyjson_doc* doc = yyjson_read_file(path.c_str(), 0, nullptr, nullptr);
    if (!doc) {
        error = "failed to read or parse JSON file: " + path;
        return std::nullopt;
    }

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        error = "JSON root must be an object";
        return std::nullopt;
    }

    ComparisonConfig cfg;

    yyjson_val* baseline_val = yyjson_obj_get(root, "baseline");
    if (!baseline_val || !yyjson_is_str(baseline_val)) {
        yyjson_doc_free(doc);
        error = "missing required string field 'baseline'";
        return std::nullopt;
    }
    cfg.baseline = yyjson_get_str(baseline_val);

    yyjson_val* variant_val = yyjson_obj_get(root, "variant");
    if (!variant_val || !yyjson_is_str(variant_val)) {
        yyjson_doc_free(doc);
        error = "missing required string field 'variant'";
        return std::nullopt;
    }
    cfg.variant = yyjson_get_str(variant_val);

    yyjson_val* defaults_val = yyjson_obj_get(root, "defaults");
    if (defaults_val && yyjson_is_obj(defaults_val)) {
        yyjson_val* dm = yyjson_obj_get(defaults_val, "metrics");
        if (dm && yyjson_is_arr(dm)) {
            cfg.defaults.metrics.clear();
            std::size_t idx, max;
            yyjson_val* elem;
            yyjson_arr_foreach(dm, idx, max, elem) {
                if (yyjson_is_str(elem)) {
                    cfg.defaults.metrics.push_back(yyjson_get_str(elem));
                }
            }
        }

        yyjson_val* dp = yyjson_obj_get(defaults_val, "percentiles");
        if (dp && yyjson_is_arr(dp)) {
            cfg.defaults.percentiles.clear();
            std::size_t idx, max;
            yyjson_val* elem;
            yyjson_arr_foreach(dp, idx, max, elem) {
                if (yyjson_is_num(elem)) {
                    cfg.defaults.percentiles.push_back(yyjson_get_num(elem));
                }
            }
        }

        yyjson_val* dt = yyjson_obj_get(defaults_val, "threshold_pct");
        if (dt && yyjson_is_num(dt)) {
            cfg.defaults.threshold_pct = yyjson_get_num(dt);
        }

        yyjson_val* ti = yyjson_obj_get(defaults_val, "time_interval_ms");
        if (ti && yyjson_is_num(ti)) {
            cfg.defaults.time_interval_ms = yyjson_get_num(ti);
        }

        yyjson_val* ds = yyjson_obj_get(defaults_val, "sort_by");
        if (ds && yyjson_is_str(ds)) {
            cfg.defaults.sort_by = yyjson_get_str(ds);
        }
    }

    yyjson_val* nodes_val = yyjson_obj_get(root, "nodes");
    if (nodes_val && yyjson_is_arr(nodes_val)) {
        std::size_t idx, max;
        yyjson_val* node_elem;
        yyjson_arr_foreach(nodes_val, idx, max, node_elem) {
            ComparisonNode node;
            if (!parse_node(node_elem, node, error)) {
                yyjson_doc_free(doc);
                return std::nullopt;
            }
            cfg.nodes.push_back(std::move(node));
        }
    }

    yyjson_doc_free(doc);
    return cfg;
}

// static
ComparisonConfig ComparisonConfig::from_cli(const std::string& baseline,
                                            const std::string& variant,
                                            const std::string& query,
                                            const std::string& group_by_str) {
    ComparisonConfig cfg;
    cfg.baseline = baseline;
    cfg.variant = variant;

    ComparisonNode node;
    node.name = "root";
    node.query = query.empty() ? R"(cat == "POSIX" OR cat == "STDIO")" : query;
    node.group_by =
        group_by_str.empty() ? split_csv("cat,name") : split_csv(group_by_str);

    cfg.nodes.push_back(std::move(node));
    return cfg;
}

void ComparisonConfig::resolve_node(
    ComparisonNode& node, const std::string& parent_query,
    const std::vector<std::string>& parent_metrics,
    const std::vector<double>& parent_percentiles, double parent_threshold,
    const std::string& parent_sort_by) {
    if (node.query.empty()) {
        node.composed_query = parent_query;
    } else if (parent_query.empty()) {
        node.composed_query = node.query;
    } else {
        node.composed_query = "(" + parent_query + ") AND (" + node.query + ")";
    }

    node.resolved_metrics = node.metrics.value_or(parent_metrics);
    node.resolved_percentiles = node.percentiles.value_or(parent_percentiles);
    node.resolved_threshold_pct = node.threshold_pct.value_or(parent_threshold);
    node.resolved_sort_by = node.sort_by.value_or(parent_sort_by);

    for (auto& child : node.children) {
        resolve_node(child, node.composed_query, node.resolved_metrics,
                     node.resolved_percentiles, node.resolved_threshold_pct,
                     node.resolved_sort_by);
    }
}

void ComparisonConfig::resolve() {
    for (auto& node : nodes) {
        resolve_node(node, "", defaults.metrics, defaults.percentiles,
                     defaults.threshold_pct, defaults.sort_by);
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::comparator
