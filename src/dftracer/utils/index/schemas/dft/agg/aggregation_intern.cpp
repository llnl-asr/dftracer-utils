// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>

#include <atomic>
#include <mutex>

namespace dftracer::utils::index::schemas::dft::agg {

namespace {
struct Registry {
    std::mutex mutex;
    dstd::Optional<dstd::StringViewMap<dstd::WeakPtr<AggInternTable>>> slot;

    dstd::StringViewMap<dstd::WeakPtr<AggInternTable>>& tables() {
        if (!slot) slot.emplace();
        return *slot;
    }
};

Registry& registry() {
    // A weak reference keeps a table's whole block alive, which the memory
    // exit check counts as a leak.
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, [] {
            Registry& r = dstd::Singleton<Registry>::get();
            std::lock_guard lock(r.mutex);
            r.slot.reset();
        });
        return true;
    }();
    return dstd::Singleton<Registry>::get();
}

std::atomic<bool>& deterministic_ids() {
    static std::atomic<bool> enabled{false};
    return enabled;
}

AggInternPtr new_table() {
    auto table = dstd::make_shared<AggInternTable>();
    if (deterministic_ids().load(std::memory_order_acquire)) {
        table->intern.enable_deterministic_ids();
    }
    return table;
}
}  // namespace

AggInternPtr intern_for_index(dstd::StringView index_path) {
    if (index_path.empty()) return make_intern_table();

    auto& r = registry();
    std::lock_guard lock(r.mutex);
    auto& tables = r.tables();
    auto it = tables.find(index_path);
    if (it != tables.end()) {
        if (auto existing = it->second.lock()) return existing;
        tables.erase(it);
    }
    auto table = new_table();
    tables.emplace(dstd::String(index_path), table);
    return table;
}

AggInternPtr make_intern_table() { return new_table(); }

void enable_deterministic_intern_ids() {
    deterministic_ids().store(true, std::memory_order_release);
}

}  // namespace dftracer::utils::index::schemas::dft::agg
