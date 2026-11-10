// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_CONFIG_H
#define DFTRACER_UTILS_PLUGINS_CONFIG_H

#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>

#include <cstdint>

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

    // An error with code Io (unreadable file), Parse (bad JSON) or
    // InvalidArgument (a root that is not an object).
    static dstd::Result<ConfigTree> from_json_file(dstd::StringView path);

    // An error with code Parse (bad JSON) or InvalidArgument (a root that is
    // not an object).
    static dstd::Result<ConfigTree> from_json_string(dstd::StringView json);

    // Set a leaf at the dotted key, inferring its type from raw_value.
    void set(dstd::StringView dotted_key, dstd::StringView raw_value);

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
        dstd::String str;
        dstd::Vector<Node> items;
        dstd::Vector<dstd::Pair<dstd::String, Node>> members;
    };

    static dstd::Result<Node> build_node(const simdjson::dom::element& el);
    static Node* find_member(Node& obj, dstd::StringView key);
    static Node infer_leaf(dstd::StringView raw_value);
    static void deep_merge(Node& dst, const Node& src);

    void materialize() const;
    void materialize_into(const Node& node, dftu_value* out) const;

    Node root_node_;

    mutable bool dirty_ = true;
    mutable dstd::Deque<dstd::String> strings_;
    mutable dstd::Deque<dstd::Vector<dftu_value>> value_arrays_;
    mutable dstd::Deque<dstd::Vector<dftu_member>> member_arrays_;
    mutable dstd::UniquePtr<dftu_value> root_;
};

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_CONFIG_H
