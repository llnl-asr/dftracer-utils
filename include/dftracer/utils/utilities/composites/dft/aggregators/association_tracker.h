#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_ASSOCIATION_TRACKER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_ASSOCIATION_TRACKER_H

#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/composites/dft/args_map.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

struct BoundaryInterval {
    std::string name;
    std::string value;
    std::uint64_t start_ts;
    std::uint64_t end_ts;
};

class AssociationTracker {
   private:
    std::unordered_map<std::uint64_t, std::uint64_t> process_parents_;
    std::unordered_set<std::uint64_t> all_pids_;
    std::unordered_map<std::uint64_t, std::vector<BoundaryInterval>>
        process_intervals_;
    std::vector<BoundaryInterval> all_intervals_;
    std::unordered_map<std::string, std::uint64_t> auto_increment_counters_;

   public:
    AssociationTracker() = default;

    void extract_from_event(std::string_view name, std::uint64_t pid,
                            std::uint64_t ts, std::uint64_t dur,
                            const ArgsMap& args,
                            const AggregationConfig& config);
    void finalize();

    std::uint64_t get_parent_pid(std::uint64_t pid) const;
    std::unordered_map<std::string, std::string> get_boundary_associations(
        std::uint64_t pid, std::uint64_t ts) const;

    const std::vector<BoundaryInterval>& get_all_intervals() const {
        return all_intervals_;
    }

    bool has_boundary_events() const { return !all_intervals_.empty(); }
    bool has_process_tree() const { return !process_parents_.empty(); }

    std::unordered_set<std::uint64_t> get_root_pids() const;
    void merge(const AssociationTracker& other);

    std::string serialize() const;
    static AssociationTracker deserialize(std::string_view data);
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_ASSOCIATION_TRACKER_H
