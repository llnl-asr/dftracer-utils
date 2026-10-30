#ifndef DFTRACER_UTILS_INDEX_EVENT_COST_H
#define DFTRACER_UTILS_INDEX_EVENT_COST_H

#include <atomic>
#include <cstdint>

namespace dftracer::utils::index::event_cost {

/// Schema leaf paths interned by index builds so far. For tests.
inline std::atomic<std::uint64_t> g_leaf_path_interns{0};

/// Chunk statistics lookups by key string instead of interned id. For tests.
inline std::atomic<std::uint64_t> g_stats_string_lookups{0};

}  // namespace dftracer::utils::index::event_cost

#endif  // DFTRACER_UTILS_INDEX_EVENT_COST_H
