// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_CONFIG_H
#define DFTRACER_UTILS_PLUGINS_CONFIG_H

#include <dftracer/utils/plugins/abi.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace simdjson {
namespace dom {
class element;
}
}  // namespace simdjson

namespace dftracer::utils::plugins {

// Owns a dftu_value config tree built from a JSON file and/or --parg overrides;
// root() materializes the internal Node tree into an ABI-shaped dftu_value
// tree.
class ConfigTree {
   public:
    ConfigTree() = default;

    ConfigTree(ConfigTree&&) = default;
    ConfigTree& operator=(ConfigTree&&) = default;
    ConfigTree(const ConfigTree&) = delete;
    ConfigTree& operator=(const ConfigTree&) = delete;

    // Throws std::runtime_error on an I/O/parse error or a non-object root.
    static ConfigTree from_json_file(const std::string& path);

    // Throws std::runtime_error on a parse error or a non-object root.
    static ConfigTree from_json_string(const std::string& json);

    // Set a leaf at the dotted key, inferring its type from raw_value.
    void set(std::string_view dotted_key, std::string_view raw_value);

    // Deep merge: object+object merges recursively, else other replaces.
    void merge_from(const ConfigTree& other);

    // Materialized root; never null, stable until the next set/merge_from.
    const dftu_value* root() const;

   private:
    struct Node {
        dftu_value_kind kind = DFTU_VAL_OBJECT;
        bool b = false;
        std::int64_t i64 = 0;
        double f64 = 0.0;
        std::string str;
        std::vector<Node> items;
        std::vector<std::pair<std::string, Node>> members;
    };

    static Node build_node(const simdjson::dom::element& el);
    static Node* find_member(Node& obj, std::string_view key);
    static Node infer_leaf(std::string_view raw_value);
    static void deep_merge(Node& dst, const Node& src);

    void materialize() const;
    void materialize_into(const Node& node, dftu_value* out) const;

    Node root_node_;

    mutable bool dirty_ = true;
    mutable std::deque<std::string> strings_;
    mutable std::deque<std::vector<dftu_value>> value_arrays_;
    mutable std::deque<std::vector<dftu_member>> member_arrays_;
    mutable std::unique_ptr<dftu_value> root_;
};

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_CONFIG_H
