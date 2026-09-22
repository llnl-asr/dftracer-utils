#ifndef DFTRACER_UTILS_SERVER_VIZ_CALLTREE_H
#define DFTRACER_UTILS_SERVER_VIZ_CALLTREE_H

// Call-tree (flame) presentation: serialize a folded-tree arena to the nested
// JSON the web UI draws. The scan + fold + per-group rooting now come from the
// View engine (View::flamegraph_partial); this header only turns the resulting
// arena into JSON. Internal to the server.

#include <dftracer/utils/dataframe/flame_arena.h>
#include <simdjson.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace dftracer::utils::server {

// The folded-tree node/arena and its (de)serialization live in the dataframe
// layer so the server, the columnar flamegraph() op, and the CLI share one
// core.
using dataframe::deserialize_flame_arena;
using dataframe::FlameNode;

// Emit one arena node (and its subtree) as JSON, children sorted by total.
inline void serialize_flame_node(simdjson::builder::string_builder& sb,
                                 std::vector<FlameNode>& arena,
                                 std::uint32_t idx) {
    FlameNode& n = arena[idx];
    sb.start_object();
    sb.append_key_value("name", n.name);
    sb.append_comma();
    sb.append_key_value("total", n.total);
    sb.append_comma();
    sb.append_key_value("self", n.self < 0 ? 0.0 : n.self);
    sb.append_comma();
    sb.append_key_value("count", n.count);
    std::sort(n.children.begin(), n.children.end(),
              [&arena](std::uint32_t a, std::uint32_t b) {
                  return arena[a].total > arena[b].total;
              });
    sb.append_comma();
    sb.escape_and_append_with_quotes("children");
    sb.append_colon();
    sb.start_array();
    for (std::size_t i = 0; i < n.children.size(); ++i) {
        if (i > 0) sb.append_comma();
        serialize_flame_node(sb, arena, n.children[i]);
    }
    sb.end_array();
    sb.end_object();
}

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_VIZ_CALLTREE_H
