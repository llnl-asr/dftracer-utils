#ifndef DFTRACER_UTILS_TRACE_INTERNAL_PARSE_WALKS_H
#define DFTRACER_UTILS_TRACE_INTERNAL_PARSE_WALKS_H

#include <atomic>
#include <cstdint>

namespace dftracer::utils::trace::internal::parse_walks {

/// While set, the index parse counts the key walks it makes. For tests.
inline std::atomic<bool> g_count{false};

/// Walks over a record's top-level object, and over its args object. A
/// record is walked once and its args are walked once.
inline std::atomic<std::uint64_t> g_record_walks{0};
inline std::atomic<std::uint64_t> g_args_walks{0};

inline void record_walk() {
    if (g_count.load(std::memory_order_relaxed))
        g_record_walks.fetch_add(1, std::memory_order_relaxed);
}

inline void args_walk() {
    if (g_count.load(std::memory_order_relaxed))
        g_args_walks.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace dftracer::utils::trace::internal::parse_walks

#endif  // DFTRACER_UTILS_TRACE_INTERNAL_PARSE_WALKS_H
