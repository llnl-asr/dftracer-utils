#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_resolver_utility.h>

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>

namespace dftracer::utils::utilities::composites::dft::aggregators {

coro::CoroTask<AssociationResolverOutput> AssociationResolverUtility::process(
    const AssociationResolverInput& input) {
    DFTRACER_UTILS_LOG_INFO(
        "Resolving associations globally from %zu trackers...",
        input.trackers.size());

    AssociationResolverOutput output;
    // NOTE(perf): move from input, the utility interface requires const& but
    // callers don't use the input after process() returns. The const_cast +
    // move avoids copying the entire aggregation map (millions of entries).
    output.aggregations =
        std::move(const_cast<AssociationResolverInput&>(input).aggregations);

    if (input.trackers.empty() || (!input.config.track_process_parents &&
                                   input.config.boundary_events.empty())) {
        DFTRACER_UTILS_LOG_INFO(
            "No associations to resolve (trackers or config disabled)");
        co_return output;
    }

    AssociationTracker global_tracker;
    for (const auto& tracker : input.trackers) {
        if (tracker) {
            global_tracker.merge(*tracker);
        }
    }

    global_tracker.finalize();

    DFTRACER_UTILS_LOG_INFO(
        "Global tracker merged: %s process relationships, %s boundary "
        "events",
        global_tracker.has_process_tree() ? "has" : "no",
        global_tracker.has_boundary_events() ? "has" : "no");

    auto root_pids = global_tracker.get_root_pids();
    if (!root_pids.empty()) {
        DFTRACER_UTILS_LOG_INFO("Found %zu root process(es):",
                                root_pids.size());
        std::size_t count = 0;
        for (std::uint64_t pid : root_pids) {
            if (count < 5) {
                DFTRACER_UTILS_LOG_INFO("  Root PID: %lu", pid);
                count++;
            }
        }
        if (root_pids.size() > 5) {
            DFTRACER_UTILS_LOG_INFO("  ... and %zu more", root_pids.size() - 5);
        }
    }

    std::size_t metrics_updated = 0;

    for (auto& [key, metrics] : output.aggregations.aggregations) {
        bool updated = false;

        if (input.config.track_process_parents &&
            global_tracker.has_process_tree()) {
            std::uint64_t parent = global_tracker.get_parent_pid(key.pid);
            if (parent != 0) {
                metrics.parent_pid = parent;
                updated = true;
            }
        }

        if (!input.config.boundary_events.empty() &&
            global_tracker.has_boundary_events()) {
            std::uint64_t representative_ts = (metrics.ts + metrics.te) / 2;

            std::uint64_t boundary_pid =
                (metrics.parent_pid > 0) ? metrics.parent_pid : key.pid;

            auto associations = global_tracker.get_boundary_associations(
                boundary_pid, representative_ts);
            if (!associations.empty()) {
                metrics.boundary_associations = std::make_unique<
                    std::unordered_map<std::string, std::string>>(
                    std::move(associations));
                updated = true;
            }
        }

        if (updated) {
            metrics_updated++;
        }
    }

    DFTRACER_UTILS_LOG_INFO(
        "Association resolution complete: %zu/%zu metrics updated",
        metrics_updated, output.aggregations.aggregations.size());

    compute_trace_metadata(global_tracker, output.aggregations, output);

    output.root_pids = root_pids;
    output.success = true;
    co_return output;
}

void AssociationResolverUtility::compute_trace_metadata(
    const AssociationTracker& tracker,
    const EventAggregatorOutput& /*aggregations*/,
    AssociationResolverOutput& output) {
    const auto& intervals = tracker.get_all_intervals();

    if (intervals.empty()) {
        DFTRACER_UTILS_LOG_INFO(
            "No boundary intervals found, skipping metadata computation");
        return;
    }

    std::unordered_map<
        std::string, std::unordered_map<
                         std::string, std::pair<std::uint64_t, std::uint64_t>>>
        ranges;

    std::uint64_t global_min = UINT64_MAX;
    std::uint64_t global_max = 0;

    for (const auto& interval : intervals) {
        global_min = std::min(global_min, interval.start_ts);
        global_max = std::max(global_max, interval.end_ts);

        auto& value_map = ranges[interval.name];
        auto& range = value_map[interval.value];

        if (range.first == 0 && range.second == 0) {
            range.first = interval.start_ts;
            range.second = interval.end_ts;
        } else {
            range.first = std::min(range.first, interval.start_ts);
            range.second = std::max(range.second, interval.end_ts);
        }
    }

    if (global_max > global_min) {
        output.trace_duration = global_max - global_min;
    }

    std::size_t total_boundaries = 0;
    for (const auto& [name, value_map] : ranges) {
        for (const auto& [value, range] : value_map) {
            BoundaryTimeRange time_range;
            time_range.ts = range.first;
            time_range.te = range.second;
            output.boundary_ranges[name][value] = time_range;
            total_boundaries++;
        }
    }

    DFTRACER_UTILS_LOG_INFO(
        "Computed trace metadata: trace_duration=%lu us, %zu boundary "
        "types, %zu total boundaries",
        output.trace_duration, output.boundary_ranges.size(), total_boundaries);
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
