// SPDX-License-Identifier: MIT
#include <dftracer/utils/domains/analysis/comparator/comparison_config.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <simdjson.h>

namespace dftracer::utils::domains::analysis::comparator {

namespace {

dstd::Vector<dstd::String> split_csv(dstd::StringView s) {
    dstd::Vector<dstd::String> result;
    while (!s.empty()) {
        const std::size_t comma = s.find(',');
        const dstd::StringView token = s.substr(0, comma);
        s = comma == dstd::StringView::npos ? dstd::StringView{}
                                            : s.substr(comma + 1);
        auto start = token.find_first_not_of(" \t");
        auto end = token.find_last_not_of(" \t");
        if (start != dstd::StringView::npos) {
            result.emplace_back(token.substr(start, end - start + 1));
        }
    }
    return result;
}

}  // namespace

// static
bool ComparisonConfig::parse_node(simdjson::dom::element val,
                                  ComparisonNode& node, dstd::String& error) {
    if (!val.is_object()) {
        error = "node must be a JSON object";
        return false;
    }

    auto name_result = val["name"];
    if (name_result.error() || !name_result.value_unsafe().is_string()) {
        error = "node missing required string field 'name'";
        return false;
    }
    node.name =
        dstd::String(name_result.value_unsafe().get_string().value_unsafe());

    auto query_result = val["duql"];
    if (!query_result.error() && query_result.value_unsafe().is_string()) {
        node.query = dstd::String(
            query_result.value_unsafe().get_string().value_unsafe());
    }

    auto gb_result = val["group_by"];
    if (!gb_result.error() && gb_result.value_unsafe().is_array()) {
        simdjson::dom::array items =
            gb_result.value_unsafe().get_array().value_unsafe();
        for (auto elem : items) {
            if (elem.is_string()) {
                node.group_by.push_back(
                    dstd::String(elem.get_string().value_unsafe()));
            }
        }
    }

    auto metrics_result = val["metrics"];
    if (!metrics_result.error() && metrics_result.value_unsafe().is_array()) {
        dstd::Vector<dstd::String> metrics;
        simdjson::dom::array items =
            metrics_result.value_unsafe().get_array().value_unsafe();
        for (auto elem : items) {
            if (elem.is_string()) {
                metrics.push_back(
                    dstd::String(elem.get_string().value_unsafe()));
            }
        }
        node.metrics = std::move(metrics);
    }

    auto pct_result = val["percentiles"];
    if (!pct_result.error() && pct_result.value_unsafe().is_array()) {
        dstd::Vector<double> percentiles;
        simdjson::dom::array items =
            pct_result.value_unsafe().get_array().value_unsafe();
        for (auto elem : items) {
            if (elem.is_double() || elem.is_int64() || elem.is_uint64()) {
                percentiles.push_back(elem.get_double().value_unsafe());
            }
        }
        node.percentiles = std::move(percentiles);
    }

    auto thr_result = val["threshold_pct"];
    if (!thr_result.error()) {
        auto thr_val = thr_result.value_unsafe();
        if (thr_val.is_double() || thr_val.is_int64() || thr_val.is_uint64()) {
            node.threshold_pct = thr_val.get_double().value_unsafe();
        }
    }

    auto sort_result = val["sort_by"];
    if (!sort_result.error() && sort_result.value_unsafe().is_string()) {
        node.sort_by = dstd::String(
            sort_result.value_unsafe().get_string().value_unsafe());
    }

    auto children_result = val["children"];
    if (!children_result.error() && children_result.value_unsafe().is_array()) {
        simdjson::dom::array items =
            children_result.value_unsafe().get_array().value_unsafe();
        for (auto child_elem : items) {
            ComparisonNode child;
            if (!parse_node(child_elem, child, error)) return false;
            node.children.push_back(std::move(child));
        }
    }

    return true;
}

// static
dstd::Optional<ComparisonConfig> ComparisonConfig::from_json_file(
    const dstd::String& path, dstd::String& error) {
    auto loaded = dstd::fs::read_file(path);
    if (!loaded) {
        error = dstd::fmt<"failed to read or parse JSON file: {}">(path);
        return dstd::nullopt;
    }
    const dstd::String& content = *loaded;

    simdjson::dom::parser parser;
    auto result = parser.parse(content.data(), content.size());
    if (result.error()) {
        error = dstd::fmt<"failed to parse JSON file: {}">(path);
        return dstd::nullopt;
    }

    auto root = result.value_unsafe();
    if (!root.is_object()) {
        error = "JSON root must be an object";
        return dstd::nullopt;
    }

    ComparisonConfig cfg;

    auto baseline_result = root["baseline"];
    if (baseline_result.error() ||
        !baseline_result.value_unsafe().is_string()) {
        error = "missing required string field 'baseline'";
        return dstd::nullopt;
    }
    cfg.baseline = dstd::String(
        baseline_result.value_unsafe().get_string().value_unsafe());

    auto variant_result = root["variant"];
    if (variant_result.error() || !variant_result.value_unsafe().is_string()) {
        error = "missing required string field 'variant'";
        return dstd::nullopt;
    }
    cfg.variant =
        dstd::String(variant_result.value_unsafe().get_string().value_unsafe());

    auto defaults_result = root["defaults"];
    if (!defaults_result.error() &&
        defaults_result.value_unsafe().is_object()) {
        auto defaults_val = defaults_result.value_unsafe();

        auto dm_result = defaults_val["metrics"];
        if (!dm_result.error() && dm_result.value_unsafe().is_array()) {
            cfg.defaults.metrics.clear();
            simdjson::dom::array items =
                dm_result.value_unsafe().get_array().value_unsafe();
            for (auto elem : items) {
                if (elem.is_string()) {
                    cfg.defaults.metrics.push_back(
                        dstd::String(elem.get_string().value_unsafe()));
                }
            }
        }

        auto dp_result = defaults_val["percentiles"];
        if (!dp_result.error() && dp_result.value_unsafe().is_array()) {
            cfg.defaults.percentiles.clear();
            simdjson::dom::array items =
                dp_result.value_unsafe().get_array().value_unsafe();
            for (auto elem : items) {
                if (elem.is_double() || elem.is_int64() || elem.is_uint64()) {
                    cfg.defaults.percentiles.push_back(
                        elem.get_double().value_unsafe());
                }
            }
        }

        auto dt_result = defaults_val["threshold_pct"];
        if (!dt_result.error()) {
            auto dt_val = dt_result.value_unsafe();
            if (dt_val.is_double() || dt_val.is_int64() || dt_val.is_uint64()) {
                cfg.defaults.threshold_pct = dt_val.get_double().value_unsafe();
            }
        }

        auto ti_result = defaults_val["time_interval_ms"];
        if (!ti_result.error()) {
            auto ti_val = ti_result.value_unsafe();
            if (ti_val.is_double() || ti_val.is_int64() || ti_val.is_uint64()) {
                cfg.defaults.time_interval_ms =
                    ti_val.get_double().value_unsafe();
            }
        }

        auto ds_result = defaults_val["sort_by"];
        if (!ds_result.error() && ds_result.value_unsafe().is_string()) {
            cfg.defaults.sort_by = dstd::String(
                ds_result.value_unsafe().get_string().value_unsafe());
        }
    }

    auto nodes_result = root["nodes"];
    if (!nodes_result.error() && nodes_result.value_unsafe().is_array()) {
        simdjson::dom::array items =
            nodes_result.value_unsafe().get_array().value_unsafe();
        for (auto node_elem : items) {
            ComparisonNode node;
            if (!parse_node(node_elem, node, error)) {
                return dstd::nullopt;
            }
            cfg.nodes.push_back(std::move(node));
        }
    }

    return cfg;
}

// static
ComparisonConfig ComparisonConfig::from_cli(const dstd::String& baseline,
                                            const dstd::String& variant,
                                            const dstd::String& query,
                                            const dstd::String& group_by_str) {
    ComparisonConfig cfg;
    cfg.baseline = baseline;
    cfg.variant = variant;

    ComparisonNode node;
    node.name = "root";
    // Empty query compares every event; an app category (FUNCTION-mode traces
    // use cat like "dynamo", not POSIX/STDIO) is only applied when the caller
    // passes one. Defaulting to a POSIX/STDIO filter silently matched zero
    // events on non-IO traces.
    node.query = query;
    node.group_by =
        group_by_str.empty() ? split_csv("cat,name") : split_csv(group_by_str);

    cfg.nodes.push_back(std::move(node));
    return cfg;
}

void ComparisonConfig::resolve_node(
    ComparisonNode& node, const dstd::String& parent_query,
    const dstd::Vector<dstd::String>& parent_metrics,
    const dstd::Vector<double>& parent_percentiles, double parent_threshold,
    const dstd::String& parent_sort_by) {
    if (node.query.empty()) {
        node.composed_query = parent_query;
    } else if (parent_query.empty()) {
        node.composed_query = node.query;
    } else {
        node.composed_query =
            dstd::fmt<"({}) AND ({})">(parent_query, node.query);
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

// static
dstd::Optional<ComparisonConfig> ComparisonConfig::from_preset(
    const dstd::String& preset, const dstd::String& baseline,
    const dstd::String& variant) {
    if (preset != "dlio") return dstd::nullopt;

    ComparisonConfig cfg;
    cfg.baseline = baseline;
    cfg.variant = variant;

    auto node = [](dstd::String name, dstd::String query,
                   dstd::Vector<ComparisonNode> children = {}) {
        ComparisonNode n;
        n.name = std::move(name);
        n.query = std::move(query);
        n.group_by = {"cat", "name"};
        n.children = std::move(children);
        return n;
    };

    cfg.nodes.push_back(node("Pipeline", R"(cat == "pipeline")",
                             {
                                 node("Epoch", R"(name == "epoch")"),
                                 node("Train", R"(name == "train")"),
                                 node("Evaluate", R"(name == "evaluate")"),
                                 node("Test", R"(name == "test")"),
                             }));

    cfg.nodes.push_back(node(
        "Data Ingestion",
        R"(cat in ["data", "dataloader", "data_loader", "reader", "generator"])",
        {
            node("Item Loading",
                 R"(cat == "data" AND name in ["item", "preprocess"])"),
            node("Batch Fetch", R"(cat == "dataloader" AND name == "fetch")"),
            node("Reader", R"(cat == "reader")"),
            node("DataLoader", R"(cat == "data_loader")"),
            node("Generator", R"(cat == "generator")"),
        }));

    cfg.nodes.push_back(
        node("Checkpointing", R"(cat == "checkpoint")",
             {
                 node("Save", R"(name in ["save_checkpoint", "capture",
                                     "checkpoint", "save_state"])"),
                 node("Load", R"(name in ["load_checkpoint", "restart",
                                     "get_tensor", "get_tensor_core"])"),
             }));

    cfg.nodes.push_back(
        node("Compute", R"(cat in ["compute", "ai_framework", "device"])",
             {
                 node("Forward/Backward",
                      R"(cat == "compute" AND name in ["forward", "backward",
                                                   "step"])"),
                 node("Framework", R"(cat == "ai_framework")"),
                 node("Device Transfer", R"(cat == "device")"),
             }));

    cfg.nodes.push_back(
        node("Storage", R"(cat == "storage")",
             {
                 node("Read",
                      R"(name in ["get_data", "get_node", "list_objects",
                              "walk_node", "isfile"])"),
                 node("Write",
                      R"(name in ["put_data", "create_node", "delete_node",
                              "create_namespace"])"),
             }));

    cfg.nodes.push_back(
        node("POSIX/STDIO I/O", R"(cat in ["POSIX", "STDIO"])",
             {
                 node("Read", trace::internal::posix_ops::name_in_query(
                                  trace::internal::posix_ops::READ)),
                 node("Write", trace::internal::posix_ops::name_in_query(
                                   trace::internal::posix_ops::WRITE)),
                 node("Metadata", trace::internal::posix_ops::name_in_query(
                                      trace::internal::posix_ops::METADATA)),
                 node("Sync", trace::internal::posix_ops::name_in_query(
                                  trace::internal::posix_ops::SYNC)),
             }));

    cfg.nodes.push_back(node("Communication", R"(cat == "comm")"));

    cfg.nodes.push_back(
        node("Benchmark", R"(cat in ["dlio_benchmark", "config"])",
             {
                 node("Lifecycle",
                      R"(cat == "dlio_benchmark" AND name in ["initialize",
                                                          "finalize"])"),
                 node("Checkpoint I/O",
                      R"(cat == "dlio_benchmark" AND name in ["_checkpoint",
                    "_checkpoint_read", "_checkpoint_write"])"),
                 node("Config", R"(cat == "config")"),
             }));

    return cfg;
}

}  // namespace dftracer::utils::domains::analysis::comparator
