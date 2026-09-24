#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_ASSOCIATION_TRACKER_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_ASSOCIATION_TRACKER_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/trace/args_map.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::schemas::dft::agg {

struct BoundaryInterval {
    std::string name;
    std::string value;
    std::uint64_t start_ts;
    std::uint64_t end_ts;
};

class AssociationTracker {
   private:
    ankerl::unordered_dense::map<std::uint64_t, std::uint64_t> process_parents_;
    ankerl::unordered_dense::set<std::uint64_t> all_pids_;
    ankerl::unordered_dense::map<std::uint64_t, std::vector<BoundaryInterval>>
        process_intervals_;
    std::vector<BoundaryInterval> all_intervals_;
    StringViewMap<std::uint64_t> auto_increment_counters_;

   public:
    AssociationTracker() = default;

    void extract_from_event(std::string_view name, std::uint64_t pid,
                            std::uint64_t ts, std::uint64_t dur,
                            const trace::ArgsMap& args,
                            const AggregationConfig& config);

    /// Accessor-driven variant of extract_from_event, byte-identical to it but
    /// reading `ret` / boundary value fields through the aggregation-core
    /// Accessor concept (arg_uint/arg_string) instead of a DOM ArgsMap, so the
    /// POD-driven AggregationFold feeds the tracker without a live DOM.
    template <class Acc>
    void extract(const Acc& acc, const AggregationConfig& config) {
        const std::uint64_t pid = acc.pid();
        if (config.track_process_parents && pid > 0) all_pids_.insert(pid);

        const std::string_view name = acc.name();
        if (config.track_process_parents &&
            (name == "fork" || name == "spawn")) {
            std::uint64_t child_pid = acc.arg_uint("ret");
            if (child_pid > 0) {
                process_parents_[child_pid] = pid;
                all_pids_.insert(child_pid);
            }
        }

        for (const auto& boundary_config : config.boundary_events) {
            if (name != boundary_config.event_name) continue;
            std::string_view value =
                acc.arg_string(boundary_config.value_field);
            std::string final_value;
            if (!value.empty()) {
                final_value = std::string(value);
            } else {
                std::string counter_key =
                    std::to_string(pid) + ":" + boundary_config.event_name;
                auto& counter = auto_increment_counters_[counter_key];
                counter++;
                final_value = std::to_string(counter);
            }
            BoundaryInterval interval;
            interval.name = boundary_config.output_name;
            interval.value = final_value;
            interval.start_ts = acc.ts();
            interval.end_ts = acc.ts() + acc.dur();
            process_intervals_[pid].push_back(interval);
            all_intervals_.push_back(interval);
        }
    }

    void finalize();

    std::uint64_t get_parent_pid(std::uint64_t pid) const;
    StringViewMap<std::string> get_boundary_associations(
        std::uint64_t pid, std::uint64_t ts) const;

    const std::vector<BoundaryInterval>& get_all_intervals() const {
        return all_intervals_;
    }

    bool has_boundary_events() const { return !all_intervals_.empty(); }
    bool has_process_tree() const { return !process_parents_.empty(); }

    ankerl::unordered_dense::set<std::uint64_t> get_root_pids() const;
    void merge(const AssociationTracker& other);

    std::string serialize() const;
    static AssociationTracker deserialize(std::string_view data);
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_ASSOCIATION_TRACKER_H
