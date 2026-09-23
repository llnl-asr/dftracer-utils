#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_NODE_REGISTRY_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_NODE_REGISTRY_H

#include <dftracer/utils/dataframe/abi.h>

#include <optional>

namespace dftracer::utils::dataframe {

/// One entry of the dftu_node_register registry: `vt` copied at registration,
/// `self` borrowed per dftu_provider_register's own contract.
struct NodeEntry {
    ::dftu_node_vt vt;
    void* self;
};

/// Looks up a registered node by name. Internal bridge so LazyFrame::op (which
/// resolves and captures the node's vt/self at the .op() call, not at every
/// later drive) can share the registry storage in node_registry.cpp without
/// exposing it through the public C ABI.
std::optional<NodeEntry> find_node(const char* name);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_NODE_REGISTRY_H
