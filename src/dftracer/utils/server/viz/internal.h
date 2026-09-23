#ifndef DFTRACER_UTILS_SERVER_VIZ_INTERNAL_H
#define DFTRACER_UTILS_SERVER_VIZ_INTERNAL_H

// Helpers shared across the viz_api modules (handlers + summary builder).
// Internal to the server; not installed.

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/trace/views/view.h>
#include <simdjson.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::server {

// The scalar fields nearly every viz per-event fold reads. `name`/`cat` borrow
// the parse buffer - copy them before the next parse on the same thread.
struct EventScalars {
    double ts = 0;
    double dur = 0;
    bool has_ts = false;
    bool has_dur = false;
    bool has_pid = false;
    std::int64_t pid = 0;
    std::int64_t tid = 0;
    std::string_view name;
    std::string_view cat;
};

// Extract the common scalars in a single object walk (cheaper than repeated
// root["x"] lookups, each of which rescans the object). Missing numeric fields
// keep 0 with has_* false; pid/tid default to 0. Replaces the hand-rolled
// per-event field extraction the viz folds used to each duplicate.
inline bool parse_event_scalars(simdjson::dom::element root,
                                EventScalars& out) {
    if (!root.is_object()) return false;
    for (auto field : root.get_object()) {
        std::string_view k = field.key;
        simdjson::dom::element v = field.value;
        if (k == "ts") {
            out.ts = json::json_number(v);
            out.has_ts = true;
        } else if (k == "dur") {
            out.dur = json::json_number(v);
            out.has_dur = true;
        } else if (k == "pid") {
            out.pid = static_cast<std::int64_t>(json::json_number(v));
            out.has_pid = true;
        } else if (k == "tid") {
            out.tid = static_cast<std::int64_t>(json::json_number(v));
        } else if (k == "name") {
            if (v.is_string()) out.name = v.get_string().value_unsafe();
        } else if (k == "cat") {
            if (v.is_string()) out.cat = v.get_string().value_unsafe();
        }
    }
    return true;
}

// Parse `event` then extract its scalars. Uses a thread-local parser, so the
// borrowed name/cat are valid only until the next parse on this thread.
inline bool parse_event_scalars(std::string_view event, EventScalars& out) {
    thread_local simdjson::dom::parser parser;
    thread_local std::string buf;
    buf.assign(event);
    auto res = parser.parse(buf);
    if (res.error()) return false;
    return parse_event_scalars(res.value_unsafe(), out);
}

// Per-name (or per-group) duration aggregate, shared by the stats fold and the
// summary builder.
struct NameStat {
    std::uint64_t count = 0;
    double total = 0;
    double min = 0;
    double max = 0;
    void merge_from(const NameStat& o) {
        if (count == 0) {
            min = o.min;
            max = o.max;
        } else if (o.count > 0) {
            min = std::min(min, o.min);
            max = std::max(max, o.max);
        }
        count += o.count;
        total += o.total;
    }
};

using NameMap = dftracer::utils::StringViewMap<NameStat>;

// Map selected index files to View sources (path + index + cached sizes).
inline std::vector<trace::views::ViewFile> to_view_files(
    const std::vector<const TraceIndex::FileInfo*>& fis) {
    std::vector<trace::views::ViewFile> v;
    v.reserve(fis.size());
    for (auto* fi : fis) {
        trace::views::ViewFile vf;
        vf.file_path = fi->path;
        vf.index_path = fi->index_path;
        vf.uncompressed_size = fi->uncompressed_size;
        vf.num_checkpoints = fi->num_checkpoints;
        vf.checkpoint_size = fi->checkpoint_size;
        v.push_back(std::move(vf));
    }
    return v;
}

// A dftracer process-spawning call (POSIX fork/clone family or the kernel
// syscalls), used to reconstruct the process hierarchy.
inline bool is_fork_syscall(std::string_view name) {
    return name == "fork" || name == "vfork" || name == "clone" ||
           name == "clone3" || name == "posix_spawn" ||
           name == "posix_spawnp" ||
           name.find("sys_clone") != std::string_view::npos ||
           name.find("sys_fork") != std::string_view::npos ||
           name.find("sys_vfork") != std::string_view::npos;
}

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_VIZ_INTERNAL_H
