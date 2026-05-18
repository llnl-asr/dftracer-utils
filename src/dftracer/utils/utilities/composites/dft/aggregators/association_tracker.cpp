#include <dftracer/utils/core/rocksdb/key_codec.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_tracker.h>

#include <algorithm>

namespace dftracer::utils::utilities::composites::dft::aggregators {

void AssociationTracker::extract_from_event(std::string_view name,
                                            std::uint64_t pid, std::uint64_t ts,
                                            std::uint64_t dur,
                                            const ArgsMap& args,
                                            const AggregationConfig& config) {
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

namespace {
namespace rocks = dftracer::utils::rocksdb;

void put_be64(std::string& out, std::uint64_t v) {
    rocks::KeyCodec::append_be64(out, v);
}
void put_be32(std::string& out, std::uint32_t v) {
    rocks::KeyCodec::append_be32(out, v);
}
void put_str(std::string& out, const std::string& s) {
    put_be32(out, static_cast<std::uint32_t>(s.size()));
    out.append(s);
}
std::uint64_t read_be64(const char*& p) {
    auto v = rocks::KeyCodec::decode_be64(std::string_view(p, 8));
    p += 8;
    return v;
}
std::uint32_t read_be32(const char*& p) {
    auto v = rocks::KeyCodec::decode_be32(std::string_view(p, 4));
    p += 4;
    return v;
}
std::string read_str(const char*& p) {
    auto len = read_be32(p);
    std::string s(p, len);
    p += len;
    return s;
}
}  // namespace

std::string AssociationTracker::serialize() const {
    std::string out;
    out.reserve(4096);

    put_be32(out, static_cast<std::uint32_t>(all_pids_.size()));
    for (auto pid : all_pids_) put_be64(out, pid);

    put_be32(out, static_cast<std::uint32_t>(process_parents_.size()));
    for (const auto& [child, parent] : process_parents_) {
        put_be64(out, child);
        put_be64(out, parent);
    }

    put_be32(out, static_cast<std::uint32_t>(all_intervals_.size()));
    for (const auto& iv : all_intervals_) {
        put_str(out, iv.name);
        put_str(out, iv.value);
        put_be64(out, iv.start_ts);
        put_be64(out, iv.end_ts);
    }

    put_be32(out, static_cast<std::uint32_t>(process_intervals_.size()));
    for (const auto& [pid, intervals] : process_intervals_) {
        put_be64(out, pid);
        put_be32(out, static_cast<std::uint32_t>(intervals.size()));
        for (const auto& iv : intervals) {
            put_str(out, iv.name);
            put_str(out, iv.value);
            put_be64(out, iv.start_ts);
            put_be64(out, iv.end_ts);
        }
    }

    return out;
}

AssociationTracker AssociationTracker::deserialize(std::string_view data) {
    AssociationTracker t;
    const char* p = data.data();

    auto num_pids = read_be32(p);
    for (std::uint32_t i = 0; i < num_pids; ++i)
        t.all_pids_.insert(read_be64(p));

    auto num_parents = read_be32(p);
    for (std::uint32_t i = 0; i < num_parents; ++i) {
        auto child = read_be64(p);
        auto parent = read_be64(p);
        t.process_parents_[child] = parent;
    }

    auto num_intervals = read_be32(p);
    t.all_intervals_.reserve(num_intervals);
    for (std::uint32_t i = 0; i < num_intervals; ++i) {
        BoundaryInterval iv;
        iv.name = read_str(p);
        iv.value = read_str(p);
        iv.start_ts = read_be64(p);
        iv.end_ts = read_be64(p);
        t.all_intervals_.push_back(std::move(iv));
    }

    auto num_pid_intervals = read_be32(p);
    for (std::uint32_t i = 0; i < num_pid_intervals; ++i) {
        auto pid = read_be64(p);
        auto count = read_be32(p);
        auto& vec = t.process_intervals_[pid];
        vec.reserve(count);
        for (std::uint32_t j = 0; j < count; ++j) {
            BoundaryInterval iv;
            iv.name = read_str(p);
            iv.value = read_str(p);
            iv.start_ts = read_be64(p);
            iv.end_ts = read_be64(p);
            vec.push_back(std::move(iv));
        }
    }

    return t;
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
