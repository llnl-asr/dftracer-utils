#ifndef DFTRACER_UTILS_INDEX_EVENT_COST_H
#define DFTRACER_UTILS_INDEX_EVENT_COST_H

#include <atomic>
#include <cstdint>

namespace dftracer::utils::index::event_cost {

/// Strings an index build interned as schema leaf paths so far. A path is
/// interned once per record shape, not once per event. For tests.
inline std::atomic<std::uint64_t> g_leaf_path_interns{0};

/// Events whose chunk statistics were found by key string rather than by
/// interned id. For tests.
inline std::atomic<std::uint64_t> g_stats_string_lookups{0};

}  // namespace dftracer::utils::index::event_cost

#endif  // DFTRACER_UTILS_INDEX_EVENT_COST_H
