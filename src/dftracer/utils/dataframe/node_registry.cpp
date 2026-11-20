// SPDX-License-Identifier: MIT
// A process-lifetime, name-keyed registry of plan nodes. LazyFrame::op
// (lazyframe.cpp) is the only reader: it resolves a node once, at the .op()
// call, and the plan keeps that node alive.

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/node_registry.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/string.h>

#include <mutex>

namespace dftracer::utils::dataframe {
namespace {

struct Nodes {
    std::mutex mutex;
    dstd::Optional<dstd::HashMap<dstd::String, dstd::SharedPtr<NodeImpl>>>
        by_name;
};

Nodes& registry() {
    Nodes& held = dstd::Singleton<Nodes>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, []() noexcept {
            Nodes& dropped = dstd::Singleton<Nodes>::get();
            const std::lock_guard<std::mutex> lock(dropped.mutex);
            dropped.by_name.reset();
        });
        return true;
    }();
    return held;
}

dstd::HashMap<dstd::String, dstd::SharedPtr<NodeImpl>>& nodes() {
    Nodes& n = registry();
    if (!n.by_name) n.by_name.emplace();
    return *n.by_name;
}

}  // namespace

dstd::Result<void> register_node(dstd::StringView name,
                                 dstd::SharedPtr<NodeImpl> impl) {
    if (name.empty() || !impl) {
        return dstd::Error::from_format<
            "node register: the name and the node must be set">(
            dstd::Condition::InvalidArgument);
    }
    std::lock_guard<std::mutex> lock(registry().mutex);
    auto& reg = nodes();
    if (reg.find(name) != reg.end()) {
        return dstd::Error::from_format<"node '{}' is already registered">(
            dstd::Condition::InvalidArgument, name);
    }
    reg.emplace(dstd::String(name), std::move(impl));
    return {};
}

bool unregister_node(dstd::StringView name) {
    dstd::SharedPtr<NodeImpl> removed;
    {
        std::lock_guard<std::mutex> lock(registry().mutex);
        auto& reg = nodes();
        auto it = reg.find(name);
        if (it == reg.end()) return false;
        removed = std::move(it->second);
        reg.erase(it);
    }
    // Released outside the lock: the node's destructor may be plugin code.
    return true;
}

dstd::String explain_line(dstd::StringView label,
                          dstd::Span<const ExplainLine> lines) {
    dstd::String out;
    if (lines.empty()) return out;
    out = dstd::fmt<"  {}:">(label);
    bool first = true;
    for (const ExplainLine& l : lines) {
        out += first ? " " : "; ";
        first = false;
        out += dstd::fmt<"{}={}">(l.key, l.value);
    }
    return out;
}

dstd::SharedPtr<NodeImpl> find_node(dstd::StringView name) {
    std::lock_guard<std::mutex> lock(registry().mutex);
    auto it = nodes().find(name);
    if (it == nodes().end()) return nullptr;
    return it->second;
}

}  // namespace dftracer::utils::dataframe

int dftu_node_unregister(const char* name) {
    if (!name) return 1;
    return dftracer::utils::dataframe::unregister_node(name) ? 0 : 1;
}
