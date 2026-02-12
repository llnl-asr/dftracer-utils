#include <dftracer/utils/utilities/composites/dft/aggregators/association_tracker.h>

#include <algorithm>

namespace dftracer::utils::utilities::composites::dft::aggregators {

void AssociationTracker::extract_from_event(const JsonValue& json,
                                            const JsonValue& args,
                                            const AggregationConfig& config) {
    std::string_view name = json["name"].get<std::string_view>();
    std::uint64_t pid = json["pid"].get<std::uint64_t>();

    if (config.track_process_parents && pid > 0) {
        all_pids_.insert(pid);
    }

    if (config.track_process_parents && (name == "fork" || name == "spawn")) {
        std::uint64_t child_pid = args["ret"].get<std::uint64_t>();
        if (child_pid > 0) {
            process_parents_[child_pid] = pid;
            all_pids_.insert(child_pid);
        }
    }

    for (const auto& boundary_config : config.boundary_events) {
        if (name == boundary_config.event_name) {
            std::string_view value =
                args[boundary_config.value_field].get<std::string_view>();

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

            std::uint64_t ts = json["ts"].get<std::uint64_t>();
            std::uint64_t dur = json["dur"].get<std::uint64_t>();

            BoundaryInterval interval;
            interval.name = boundary_config.output_name;
            interval.value = final_value;
            interval.start_ts = ts;
            interval.end_ts = ts + dur;

            process_intervals_[pid].push_back(interval);
            all_intervals_.push_back(interval);
        }
    }
}

void AssociationTracker::finalize() {
    std::sort(all_intervals_.begin(), all_intervals_.end(),
              [](const BoundaryInterval& a, const BoundaryInterval& b) {
                  return a.start_ts < b.start_ts;
              });
}

std::uint64_t AssociationTracker::get_parent_pid(std::uint64_t pid) const {
    auto it = process_parents_.find(pid);
    return (it != process_parents_.end()) ? it->second : 0;
}

std::unordered_map<std::string, std::string>
AssociationTracker::get_boundary_associations(std::uint64_t pid,
                                              std::uint64_t ts) const {
    std::unordered_map<std::string, std::string> result;

    auto it = process_intervals_.find(pid);
    if (it != process_intervals_.end()) {
        for (const auto& interval : it->second) {
            if (ts < interval.start_ts) break;

            if (ts >= interval.start_ts && ts < interval.end_ts) {
                result[interval.name] = interval.value;
            }
        }
    }

    if (result.empty()) {
        std::uint64_t parent = get_parent_pid(pid);
        if (parent != 0) {
            return get_boundary_associations(parent, ts);
        }
    }

    return result;
}

std::unordered_set<std::uint64_t> AssociationTracker::get_root_pids() const {
    std::unordered_set<std::uint64_t> roots;
    for (std::uint64_t pid : all_pids_) {
        if (process_parents_.find(pid) == process_parents_.end()) {
            roots.insert(pid);
        }
    }
    return roots;
}

void AssociationTracker::merge(const AssociationTracker& other) {
    all_pids_.insert(other.all_pids_.begin(), other.all_pids_.end());

    for (const auto& [child_pid, parent_pid] : other.process_parents_) {
        process_parents_[child_pid] = parent_pid;
    }

    for (const auto& [pid, intervals] : other.process_intervals_) {
        auto& my_intervals = process_intervals_[pid];
        my_intervals.insert(my_intervals.end(), intervals.begin(),
                            intervals.end());
    }

    all_intervals_.insert(all_intervals_.end(), other.all_intervals_.begin(),
                          other.all_intervals_.end());

    if (!all_intervals_.empty()) {
        std::sort(all_intervals_.begin(), all_intervals_.end(),
                  [](const BoundaryInterval& a, const BoundaryInterval& b) {
                      return a.start_ts < b.start_ts;
                  });
    }

    for (auto& [pid, intervals] : process_intervals_) {
        std::sort(intervals.begin(), intervals.end(),
                  [](const BoundaryInterval& a, const BoundaryInterval& b) {
                      return a.start_ts < b.start_ts;
                  });
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
