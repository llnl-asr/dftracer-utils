#include <dftracer/utils/utilities/composites/dft/comparator/comparison_config.h>
#include <simdjson.h>

#include <fstream>
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
bool ComparisonConfig::parse_node(simdjson::dom::element val,
                                  ComparisonNode& node, std::string& error) {
    if (!val.is_object()) {
        error = "node must be a JSON object";
        return false;
    }

    auto name_result = val["name"];
    if (name_result.error() || !name_result.value_unsafe().is_string()) {
        error = "node missing required string field 'name'";
        return false;
    }
    node.name = std::string(name_result.value_unsafe().get_string().value());

    auto query_result = val["query"];
    if (!query_result.error() && query_result.value_unsafe().is_string()) {
        node.query =
            std::string(query_result.value_unsafe().get_string().value());
    }

    auto gb_result = val["group_by"];
    if (!gb_result.error() && gb_result.value_unsafe().is_array()) {
        for (auto elem : gb_result.value_unsafe().get_array()) {
            if (elem.is_string()) {
                node.group_by.push_back(std::string(elem.get_string().value()));
            }
        }
    }

    auto metrics_result = val["metrics"];
    if (!metrics_result.error() && metrics_result.value_unsafe().is_array()) {
        std::vector<std::string> metrics;
        for (auto elem : metrics_result.value_unsafe().get_array()) {
            if (elem.is_string()) {
                metrics.push_back(std::string(elem.get_string().value()));
            }
        }
        node.metrics = std::move(metrics);
    }

    auto pct_result = val["percentiles"];
    if (!pct_result.error() && pct_result.value_unsafe().is_array()) {
        std::vector<double> percentiles;
        for (auto elem : pct_result.value_unsafe().get_array()) {
            if (elem.is_double() || elem.is_int64() || elem.is_uint64()) {
                percentiles.push_back(elem.get_double().value());
            }
        }
        node.percentiles = std::move(percentiles);
    }

    auto thr_result = val["threshold_pct"];
    if (!thr_result.error()) {
        auto thr_val = thr_result.value_unsafe();
        if (thr_val.is_double() || thr_val.is_int64() || thr_val.is_uint64()) {
            node.threshold_pct = thr_val.get_double().value();
        }
    }

    auto sort_result = val["sort_by"];
    if (!sort_result.error() && sort_result.value_unsafe().is_string()) {
        node.sort_by =
            std::string(sort_result.value_unsafe().get_string().value());
    }

    auto children_result = val["children"];
    if (!children_result.error() && children_result.value_unsafe().is_array()) {
        for (auto child_elem : children_result.value_unsafe().get_array()) {
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
    std::ifstream file(path);
    if (!file) {
        error = "failed to read or parse JSON file: " + path;
        return std::nullopt;
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    simdjson::dom::parser parser;
    auto result = parser.parse(content);
    if (result.error()) {
        error = "failed to parse JSON file: " + path;
        return std::nullopt;
    }

    auto root = result.value_unsafe();
    if (!root.is_object()) {
        error = "JSON root must be an object";
        return std::nullopt;
    }

    ComparisonConfig cfg;

    auto baseline_result = root["baseline"];
    if (baseline_result.error() ||
        !baseline_result.value_unsafe().is_string()) {
        error = "missing required string field 'baseline'";
        return std::nullopt;
    }
    cfg.baseline =
        std::string(baseline_result.value_unsafe().get_string().value());

    auto variant_result = root["variant"];
    if (variant_result.error() || !variant_result.value_unsafe().is_string()) {
        error = "missing required string field 'variant'";
        return std::nullopt;
    }
    cfg.variant =
        std::string(variant_result.value_unsafe().get_string().value());

    auto defaults_result = root["defaults"];
    if (!defaults_result.error() &&
        defaults_result.value_unsafe().is_object()) {
        auto defaults_val = defaults_result.value_unsafe();

        auto dm_result = defaults_val["metrics"];
        if (!dm_result.error() && dm_result.value_unsafe().is_array()) {
            cfg.defaults.metrics.clear();
            for (auto elem : dm_result.value_unsafe().get_array()) {
                if (elem.is_string()) {
                    cfg.defaults.metrics.push_back(
                        std::string(elem.get_string().value()));
                }
            }
        }

        auto dp_result = defaults_val["percentiles"];
        if (!dp_result.error() && dp_result.value_unsafe().is_array()) {
            cfg.defaults.percentiles.clear();
            for (auto elem : dp_result.value_unsafe().get_array()) {
                if (elem.is_double() || elem.is_int64() || elem.is_uint64()) {
                    cfg.defaults.percentiles.push_back(
                        elem.get_double().value());
                }
            }
        }

        auto dt_result = defaults_val["threshold_pct"];
        if (!dt_result.error()) {
            auto dt_val = dt_result.value_unsafe();
            if (dt_val.is_double() || dt_val.is_int64() || dt_val.is_uint64()) {
                cfg.defaults.threshold_pct = dt_val.get_double().value();
            }
        }

        auto ti_result = defaults_val["time_interval_ms"];
        if (!ti_result.error()) {
            auto ti_val = ti_result.value_unsafe();
            if (ti_val.is_double() || ti_val.is_int64() || ti_val.is_uint64()) {
                cfg.defaults.time_interval_ms = ti_val.get_double().value();
            }
        }

        auto ds_result = defaults_val["sort_by"];
        if (!ds_result.error() && ds_result.value_unsafe().is_string()) {
            cfg.defaults.sort_by =
                std::string(ds_result.value_unsafe().get_string().value());
        }
    }

    auto nodes_result = root["nodes"];
    if (!nodes_result.error() && nodes_result.value_unsafe().is_array()) {
        for (auto node_elem : nodes_result.value_unsafe().get_array()) {
            ComparisonNode node;
            if (!parse_node(node_elem, node, error)) {
                return std::nullopt;
            }
            cfg.nodes.push_back(std::move(node));
        }
    }

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
