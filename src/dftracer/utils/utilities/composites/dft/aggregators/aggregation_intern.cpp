#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_intern.h>

#include <atomic>
#include <mutex>

namespace dftracer::utils::utilities::composites::dft::aggregators {

namespace {
struct Registry {
    std::mutex mutex;
    StringViewMap<std::weak_ptr<AggInternTable>> tables;
};

Registry& registry() {
    static Registry r;
    return r;
}

std::atomic<bool>& deterministic_ids() {
    static std::atomic<bool> enabled{false};
    return enabled;
}

AggInternPtr new_table() {
    auto table = std::make_shared<AggInternTable>();
    if (deterministic_ids().load(std::memory_order_acquire)) {
        table->intern.enable_deterministic_ids();
    }
    return table;
}
}  // namespace

AggInternPtr intern_for_index(std::string_view index_path) {
    if (index_path.empty()) return make_intern_table();

    auto& r = registry();
    std::lock_guard lock(r.mutex);
    auto it = r.tables.find(index_path);
    if (it != r.tables.end()) {
        if (auto existing = it->second.lock()) return existing;
        r.tables.erase(it);
    }
    auto table = new_table();
    r.tables.emplace(std::string(index_path), table);
    return table;
}

AggInternPtr make_intern_table() { return new_table(); }

void enable_deterministic_intern_ids() {
    deterministic_ids().store(true, std::memory_order_release);
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
