#include <dftracer/utils/utilities/replay/replay.h>

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::replay {

void ReplayResult::merge(const ReplayResult& other) {
    total_events += other.total_events;
    executed_events += other.executed_events;
    filtered_events += other.filtered_events;
    failed_events += other.failed_events;
    total_duration += other.total_duration;
    execution_duration += other.execution_duration;
    for (const auto& [func, count] : other.function_counts)
        function_counts[func] += count;
    for (const auto& [cat, count] : other.category_counts)
        category_counts[cat] += count;
    error_messages.insert(error_messages.end(), other.error_messages.begin(),
                          other.error_messages.end());
    for (const auto& [pid, count] : other.pid_counts) pid_counts[pid] += count;
    for (const auto& [tid, count] : other.tid_counts) tid_counts[tid] += count;
    total_bytes_read += other.total_bytes_read;
    total_bytes_written += other.total_bytes_written;
    first_timestamp = std::min(first_timestamp, other.first_timestamp);
    last_timestamp = std::max(last_timestamp, other.last_timestamp);
    total_nodes += other.total_nodes;
    tree_depth = std::max(tree_depth, other.tree_depth);
    unique_processes += other.unique_processes;
}

// =============================================================================
// ReplayResult::print_summary Implementation
// =============================================================================

void ReplayResult::print_summary() const {
    std::printf("\n=== Replay Summary ===\n");
    std::printf("Total events: %zu\n", total_events);
    std::printf("Executed: %zu\n", executed_events);
    std::printf("Filtered: %zu\n", filtered_events);
    std::printf("Failed: %zu\n", failed_events);

    double success_rate = total_events > 0
                              ? (static_cast<double>(executed_events) /
                                 static_cast<double>(total_events) * 100.0)
                              : 0.0;
    std::printf("Success rate: %.2f%%\n", success_rate);

    std::printf("\nTiming:\n");
    std::printf("  Total duration: %.3f ms\n",
                static_cast<double>(total_duration.count()) / 1000.0);
    std::printf("  Execution duration: %.3f ms\n",
                static_cast<double>(execution_duration.count()) / 1000.0);

    if (first_timestamp != UINT64_MAX && last_timestamp > 0) {
        std::printf(
            "  Trace timespan: %.6f seconds\n",
            static_cast<double>(last_timestamp - first_timestamp) / 1000000.0);
    }

    std::printf("\nI/O Statistics:\n");
    std::printf("  Bytes read: %zu (%.2f MB)\n", total_bytes_read,
                static_cast<double>(total_bytes_read) / (1024.0 * 1024.0));
    std::printf("  Bytes written: %zu (%.2f MB)\n", total_bytes_written,
                static_cast<double>(total_bytes_written) / (1024.0 * 1024.0));

    std::printf("\nProcess/Thread Statistics:\n");
    std::printf("  Unique PIDs: %zu\n", pid_counts.size());
    std::printf("  Unique TIDs: %zu\n", tid_counts.size());

    if (!pid_counts.empty()) {
        std::printf("\n  Events per PID:\n");
        for (const auto& [pid, count] : pid_counts) {
            std::printf("    PID %u: %zu events\n", pid, count);
        }
    }

    if (!tid_counts.empty() && tid_counts.size() > 1) {
        std::printf("\n  Events per TID:\n");
        for (const auto& [tid, count] : tid_counts) {
            std::printf("    TID %u: %zu events\n", tid, count);
        }
    }

    if (!function_counts.empty()) {
        std::printf("\n  Top functions by count:\n");
        // function_counts keys are string_views into the replay intern
        // pool; sorting needs an indexable copy. Keep the views to avoid
        // re-allocating strings for the dictionary entries (read,
        // write, ...).
        std::vector<std::pair<std::string_view, std::size_t>> sorted_funcs(
            function_counts.begin(), function_counts.end());
        std::sort(
            sorted_funcs.begin(), sorted_funcs.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

        std::size_t max_display =
            std::min(sorted_funcs.size(), std::size_t(10));
        for (std::size_t i = 0; i < max_display; i++) {
            std::printf("    %-30.*s: %zu\n",
                        static_cast<int>(sorted_funcs[i].first.size()),
                        sorted_funcs[i].first.data(), sorted_funcs[i].second);
        }
    }

    if (!category_counts.empty()) {
        std::printf("\n  Events per category:\n");
        for (const auto& [cat, count] : category_counts) {
            std::printf("    %-20.*s: %zu\n", static_cast<int>(cat.size()),
                        cat.data(), count);
        }
    }

    if (!error_messages.empty()) {
        std::printf("\n=== Errors (%zu total) ===\n", error_messages.size());
        std::size_t max_errors =
            std::min(error_messages.size(), std::size_t(10));
        for (std::size_t i = 0; i < max_errors; i++) {
            std::printf("  %s\n", error_messages[i].c_str());
        }
        if (error_messages.size() > 10) {
            std::printf("  ... and %zu more errors\n",
                        error_messages.size() - 10);
        }
    }

    std::printf("=====================\n");
}

}  // namespace dftracer::utils::utilities::replay
