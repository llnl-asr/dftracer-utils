// dftu_node_register/unregister: a process-lifetime, name-keyed registry of
// plan-node vtables, mirroring provider_registry.cpp. LazyFrame::op
// (lazyframe.cpp) is the only reader: it resolves and captures a node's
// vt/self once, at the .op() call, and drives every later cursor open()
// straight against that captured pair rather than re-querying this registry.

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/node_registry.h>

#include <mutex>
#include <string>
#include <unordered_map>

namespace dftracer::utils::dataframe {
namespace {

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, NodeEntry>& nodes() {
    static std::unordered_map<std::string, NodeEntry> m;
    return m;
}

}  // namespace

std::optional<NodeEntry> find_node(const char* name) {
    if (!name) return std::nullopt;
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto it = nodes().find(name);
    if (it == nodes().end()) return std::nullopt;
    return it->second;
}

}  // namespace dftracer::utils::dataframe

extern "C" {

int dftu_node_register(const char* name, const dftu_node_vt* vt, void* self) {
    if (!name || !vt || !vt->output_schema || !vt->open) return 1;
    using dftracer::utils::dataframe::NodeEntry;
    std::lock_guard<std::mutex> lock(
        dftracer::utils::dataframe::registry_mutex());
    auto& reg = dftracer::utils::dataframe::nodes();
    if (reg.find(name) != reg.end()) return 1;
    reg.emplace(name, NodeEntry{*vt, self});
    return 0;
}

int dftu_node_unregister(const char* name) {
    if (!name) return 1;
    dftracer::utils::dataframe::NodeEntry removed;
    {
        std::lock_guard<std::mutex> lock(
            dftracer::utils::dataframe::registry_mutex());
        auto& reg = dftracer::utils::dataframe::nodes();
        auto it = reg.find(name);
        if (it == reg.end()) return 1;
        removed = it->second;
        reg.erase(it);
    }
    // Outside the lock: destroy() is caller code (a plugin's), which must not
    // run while the registry lock is held.
    if (removed.vt.destroy) removed.vt.destroy(removed.self);
    return 0;
}

}  // extern "C"
