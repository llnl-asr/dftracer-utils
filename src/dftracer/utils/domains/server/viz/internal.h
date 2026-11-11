// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DOMAINS_SERVER_VIZ_INTERNAL_H
#define DFTRACER_UTILS_DOMAINS_SERVER_VIZ_INTERNAL_H

// Helpers shared across the viz_api modules (handlers + summary builder).
// Internal to the server; not installed.

#include <dftracer/utils/domains/server/trace_index.h>
#include <dftracer/utils/domains/server/viz/record_event.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>
#include <dftracer/utils/trace/views/view.h>
#include <simdjson.h>

#include <algorithm>
#include <cstdint>

namespace dftracer::utils::domains::server {

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
    dstd::StringView name;
    dstd::StringView cat;
};

// Extract the common scalars in a single object walk (cheaper than repeated
// root["x"] lookups, each of which rescans the object). Missing numeric fields
// keep 0 with has_* false; pid/tid default to 0. Replaces the hand-rolled
// per-event field extraction the viz folds used to each duplicate.
inline bool parse_event_scalars(simdjson::dom::element root,
                                EventScalars& out) {
    if (!root.is_object()) return false;
    simdjson::dom::object items = root.get_object().value_unsafe();
    for (auto field : items) {
        dstd::StringView k = field.key;
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

// The number `v` holds as a double: an integer or a real, a signed infinity
// for one too large for a double; 0 for anything else.
inline double value_number(json::Value& v) {
    switch (v.type()) {
        case json::Value::Type::Int: {
            const auto i = v.as_int();
            return i ? static_cast<double>(*i) : 0;
        }
        case json::Value::Type::Double: {
            const auto d = v.as_double();
            return d ? *d : 0;
        }
        default:
            return 0;
    }
}

// As the DOM overload, reading `root` on demand in one forward walk. A key
// that repeats keeps its first value, as a key lookup does. A read error marks
// `root` broken, which the caller checks before it keeps a result.
inline bool parse_event_scalars(json::Value& root, EventScalars& out) {
    if (root.type() != json::Value::Type::Object) return false;
    bool seen_tid = false, seen_name = false, seen_cat = false;
    root.for_each_member([&](dstd::StringView k, json::Value v) {
        if (k == "ts") {
            if (out.has_ts) return;
            out.ts = value_number(v);
            out.has_ts = true;
        } else if (k == "dur") {
            if (out.has_dur) return;
            out.dur = value_number(v);
            out.has_dur = true;
        } else if (k == "pid") {
            if (out.has_pid) return;
            out.pid = static_cast<std::int64_t>(value_number(v));
            out.has_pid = true;
        } else if (k == "tid") {
            if (seen_tid) return;
            seen_tid = true;
            out.tid = static_cast<std::int64_t>(value_number(v));
        } else if (k == "name") {
            if (seen_name) return;
            seen_name = true;
            if (const auto s = v.as_string()) out.name = *s;
        } else if (k == "cat") {
            if (seen_cat) return;
            seen_cat = true;
            if (const auto s = v.as_string()) out.cat = *s;
        }
    });
    return !json::detail::ValueAccess::broken(root);
}

// Parse `event` then extract its scalars. Uses a thread-local document, so the
// borrowed name/cat are valid only until the next parse on this thread.
inline bool parse_event_scalars(dstd::StringView event, EventScalars& out) {
    thread_local json::Document document;
    auto parsed = document.parse(event);
    return parsed && parse_event_scalars(*parsed, out);
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

using NameMap = dftracer::utils::dstd::StringViewMap<NameStat>;

// Map selected index files to view sources (path + index + cached sizes).
inline dstd::Vector<trace::views::ViewFile> to_view_files(
    const dstd::Vector<const TraceIndex::FileInfo*>& fis) {
    dstd::Vector<trace::views::ViewFile> v;
    v.reserve(fis.size());
    for (auto* fi : fis) {
        trace::views::ViewFile vf;
        vf.file_path = fi->path;
        vf.index_path = fi->index_path;
        vf.uncompressed_size = fi->uncompressed_size;
        vf.checkpoint_count = fi->checkpoint_count;
        vf.checkpoint_size = fi->checkpoint_size;
        v.push_back(std::move(vf));
    }
    return v;
}

// A dftracer process-spawning call (POSIX fork/clone family or the kernel
// syscalls), used to reconstruct the process hierarchy.
inline bool is_fork_syscall(dstd::StringView name) {
    return name == "fork" || name == "vfork" || name == "clone" ||
           name == "clone3" || name == "posix_spawn" ||
           name == "posix_spawnp" ||
           name.find("sys_clone") != dstd::StringView::npos ||
           name.find("sys_fork") != dstd::StringView::npos ||
           name.find("sys_vfork") != dstd::StringView::npos;
}

}  // namespace dftracer::utils::domains::server

#endif  // DFTRACER_UTILS_DOMAINS_SERVER_VIZ_INTERNAL_H
