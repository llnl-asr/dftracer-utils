// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_SERVER_VIZ_DENSITY_H
#define DFTRACER_UTILS_SERVER_VIZ_DENSITY_H

// Density and counter per-event primitives: the DensityMap fold, the I/O
// CounterAcc fold, and the ph="C" counter-series fold, plus group-value
// extraction and containment-depth assignment. Internal to the server.

#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/reserved_args.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>
#include <dftracer/utils/server/viz/internal.h>
#include <dftracer/utils/server/viz/record_event.h>
#include <simdjson.h>

#include <cmath>
#include <cstdint>
#include <string_view>

namespace dftracer::utils::server {

using json::json_number;

// Sub-pixel events are bucketed by (pid, tid, pixel-column) instead of dropped,
// so zoomed-out views still show where activity is. The live path also assigns
// each bucket a containment depth (see assign_view_depths) so blocks stack
// under their enclosing events instead of collapsing to row 0.
struct DensityKey {
    std::int64_t pid;
    std::int64_t tid;
    std::int64_t col;
    dstd::String group;  // group_by value; "" when grouping is off or missing
    bool operator==(const DensityKey& o) const {
        return pid == o.pid && tid == o.tid && col == o.col && group == o.group;
    }
};

struct DensityKeyHash {
    std::uint64_t operator()(const DensityKey& k) const noexcept {
        std::uint64_t h =
            dftracer::utils::dstd::hash::FNV1A_OFFSET_BASIS_LEGACY;
        auto mix = [&](std::uint64_t v) {
            h ^= v;
            h *= dftracer::utils::dstd::hash::FNV1A_PRIME;
        };
        mix(static_cast<std::uint64_t>(k.pid));
        mix(static_cast<std::uint64_t>(k.tid));
        mix(static_cast<std::uint64_t>(k.col));
        mix(dstd::Hash<dstd::String>{}(k.group));
        return h;
    }
};

struct DensityAgg {
    std::uint32_t count = 0;
    double total = 0;
    double max_dur = -1;
    std::uint32_t depth = 0;  // containment depth, set by assign_view_depths
    dstd::String
        name;  // representative: name of the longest event in the bucket
    // ph="C" counter aggregation: a bucket carries the summed sample value
    // (mean = value_sum / count) so the block can surface the reading on hover.
    bool counter = false;
    double value_sum = 0;
    void merge_from(const DensityAgg& o) {
        count += o.count;
        total += o.total;
        value_sum += o.value_sum;
        counter = counter || o.counter;
        if (o.max_dur > max_dur) {
            max_dur = o.max_dur;
            name = o.name;
        }
    }
};

using DensityMap = dstd::HashMap<DensityKey, DensityAgg, DensityKeyHash>;

// Composite group-value separator: joins the per-column values of a multi-key
// group_by ("cat,fhash") into one lane key. Never appears in field values.
static constexpr char GROUP_SEP = '\x1f';

// Value of `col` in the event for group_by, as a display string. Dotted paths
// walk nested objects; a path the event lacks is looked up under "args" (the
// canonical home of domain fields, and a path record's own fields). Anything
// missing or non-scalar yields "" so events are never dropped - the client
// renders those under "(none)".
inline dstd::String extract_one_group_value(simdjson::dom::element root,
                                            dstd::StringView col) {
    if (auto v = find_path(root, col)) return scalar_text(*v);
    auto args = root["args"];
    if (args.error()) return "";
    if (auto v = find_path(args.value_unsafe(), col)) return scalar_text(*v);
    return "";
}

// The value at dotted `path` under `root`, read on demand: `fn(value)` runs
// on it and its result comes back; false when the path is absent. Empty
// segments are skipped, and every step but the last needs an object.
template <typename Fn>
inline bool with_value_at(json::Value& root, dstd::StringView path, Fn&& fn) {
    dstd::Optional<json::Value> holder;
    json::Value* cur = &root;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto dot = path.find('.', start);
        const auto key = path.substr(start, dot == dstd::StringView::npos
                                                ? path.size() - start
                                                : dot - start);
        if (!key.empty()) {
            if (cur->type() != json::Value::Type::Object) return false;
            auto next = cur->find(key);
            if (!next) return false;
            holder = std::move(next);
            cur = &*holder;
        }
        if (dot == dstd::StringView::npos) break;
        start = dot + 1;
    }
    return fn(*cur);
}

// A scalar as display text; "" for null, objects and arrays. An integer past
// int64 keeps its digits.
inline dstd::String scalar_text(json::Value& v) {
    switch (v.type()) {
        case json::Value::Type::String: {
            const auto s = v.as_string();
            return s ? dstd::String(*s) : dstd::String();
        }
        case json::Value::Type::Int: {
            const auto i = v.as_int();
            return i ? dstd::to_string(*i) : dstd::String();
        }
        case json::Value::Type::Double: {
            const dstd::StringView token = v.raw();
            if (!token.empty() && token.front() != '-' &&
                token.find_first_of(".eE") == dstd::StringView::npos)
                return dstd::String(token);
            const auto d = v.as_double();
            return d ? dstd::to_string(*d) : dstd::String();
        }
        case json::Value::Type::Bool: {
            const auto b = v.as_bool();
            return b && *b ? "true" : "false";
        }
        default:
            return "";
    }
}

inline dstd::String extract_one_group_value(json::Value& root,
                                            dstd::StringView col) {
    dstd::String out;
    auto text = [&out](json::Value& v) {
        out = scalar_text(v);
        return true;
    };
    if (with_value_at(root, col, text)) return out;
    auto args = root.find("args");
    if (!args) return "";
    if (with_value_at(*args, col, text)) return out;
    return "";
}

// Group value for one or more columns. A comma-separated `col` ("cat,fhash")
// yields the per-column values joined by GROUP_SEP so lanes split by the tuple;
// each component keeps its raw value (hashes stay hashes) for client-side
// resolution.
template <typename Root>
inline dstd::String extract_group_value(Root&& root, dstd::StringView col) {
    if (col.find(',') == dstd::StringView::npos)
        return extract_one_group_value(root, col);
    dstd::String out;
    std::size_t start = 0;
    bool first = true;
    while (start <= col.size()) {
        auto comma = col.find(',', start);
        auto part = col.substr(start, comma == dstd::StringView::npos
                                          ? col.size() - start
                                          : comma - start);
        if (!first) out.push_back(GROUP_SEP);
        first = false;
        out += extract_one_group_value(root, part);
        if (comma == dstd::StringView::npos) break;
        start = comma + 1;
    }
    return out;
}

inline dstd::String extract_group_from_line(dstd::StringView event,
                                            dstd::StringView col) {
    thread_local dftracer::utils::json::RecordParser parser;
    thread_local dstd::String buf;
    buf.assign(event);
    auto res = parse_string(parser, buf);
    if (res.error()) res = json::parse_refused(parser, event);
    if (res.error()) return "";
    auto root = res.value_unsafe();
    if (!root.is_object()) return "";
    return extract_group_value(root, col);
}

// Aggregate one event (its scalars read, with its duration) into the density
// bucket for its (pid, tid, pixel-column, group). Used both by fold_density for
// sub-threshold events and to demote a dense window's overflow individuals.
inline void add_to_density(const EventScalars& s, double dur, double threshold,
                           double begin, DensityMap& dens, dstd::String group) {
    std::int64_t col = static_cast<std::int64_t>((s.ts - begin) / threshold);
    DensityKey k{s.pid, s.tid, col, std::move(group)};
    auto it = dens.find(k);
    if (it == dens.end()) it = dens.emplace(std::move(k), DensityAgg{}).first;
    auto& a = it->second;
    a.count += 1;
    a.total += dur;
    if (dur > a.max_dur) {
        a.max_dur = dur;
        a.name.assign(s.name);
    }
}

enum class DensityFold : std::uint8_t { Folded, Individual, Broken };

// Fold a sub-threshold event into the density map. Individual (the caller
// keeps it as an individual event) when it has no duration or is at/above
// `threshold`. Broken, with nothing added, when a read hit a syntax error.
inline DensityFold fold_density(json::Value& root, double threshold,
                                double begin, DensityMap& dens,
                                double* out_dur = nullptr,
                                dstd::StringView group_col = {}) {
    EventScalars s;
    if (!parse_event_scalars(root, s))
        return json::detail::ValueAccess::broken(root)
                   ? DensityFold::Broken
                   : DensityFold::Individual;
    if (!s.has_dur) return DensityFold::Individual;
    if (out_dur) *out_dur = s.dur;
    if (threshold <= 0 || s.dur >= threshold) return DensityFold::Individual;

    dstd::String group;
    if (!group_col.empty()) {
        group = extract_group_value(root, group_col);
        if (json::detail::ValueAccess::broken(root)) return DensityFold::Broken;
    }
    add_to_density(s, s.dur, threshold, begin, dens, std::move(group));
    return DensityFold::Folded;
}

// Demote a set of individual events (indices into `big`, already kept whole
// because dur >= threshold) into density blocks. Lets a dense window cap its
// individual-event count without dropping any activity from the view.
inline void fold_overflow_events(const dstd::Vector<dstd::String>& big,
                                 const dstd::Vector<double>& big_dur,
                                 const dstd::Vector<std::uint32_t>& overflow,
                                 double threshold, double begin,
                                 DensityMap& dens, dstd::StringView group_col) {
    json::Document document;
    for (std::uint32_t i : overflow) {
        auto parsed = document.parse(big[i]);
        if (!parsed) continue;
        EventScalars s;
        if (!parse_event_scalars(*parsed, s)) continue;
        dstd::String group;
        if (!group_col.empty()) {
            group = extract_group_value(*parsed, group_col);
            if (json::detail::ValueAccess::broken(*parsed)) continue;
        }
        add_to_density(s, big_dur[i], threshold, begin, dens, std::move(group));
    }
}

// Thin out FH/HH hash-declaration records: keep every one an event in this
// response refers to, plus a small carry of the rest so the client keeps
// building its hash map. A trace with millions of files emits these constantly
// and in a dense window they outweigh the drawable events several times over.
static constexpr std::size_t MAX_UNREFERENCED_HASH_RECORDS = 500;

inline void drop_unreferenced_hash_records(dstd::Vector<dstd::String>& big) {
    thread_local dftracer::utils::json::RecordParser parser;
    thread_local dstd::String buf;
    dstd::HashSet<dstd::String> referenced;
    dstd::Vector<bool> is_decl(big.size(), false);

    for (std::size_t i = 0; i < big.size(); ++i) {
        buf.assign(big[i]);
        auto res = parse_string(parser, buf);
        if (res.error()) res = json::parse_refused(parser, big[i]);
        if (res.error()) continue;
        auto root = res.value_unsafe();
        if (!root.is_object()) continue;
        auto nr = root["name"];
        dstd::StringView name;
        if (!nr.error() && nr.is_string())
            name = nr.get_string().value_unsafe();
        auto args = root["args"];
        if (args.error() || !args.is_object()) continue;
        if (name == "FH" || name == "HH") {
            is_decl[i] = true;
            continue;
        }
        for (const char* key : {"fhash", "hhash"}) {
            auto v = args[key];
            if (!v.error() && v.is_string())
                referenced.emplace(v.get_string().value_unsafe());
        }
    }

    std::size_t out = 0;
    std::size_t carried = 0;
    for (std::size_t i = 0; i < big.size(); ++i) {
        if (is_decl[i]) {
            buf.assign(big[i]);
            auto res = parse_string(parser, buf);
            if (res.error()) res = json::parse_refused(parser, big[i]);
            bool keep = false;
            if (!res.error()) {
                auto v = res.value_unsafe()["args"]["value"];
                if (!v.error() && v.is_string())
                    keep = referenced.count(dstd::String(
                               v.get_string().value_unsafe())) != 0;
            }
            if (!keep) {
                if (carried >= MAX_UNREFERENCED_HASH_RECORDS) continue;
                ++carried;
            }
        }
        if (out != i) big[out] = std::move(big[i]);
        ++out;
    }
    big.resize(out);
}

// Read just the ts and dur of an event. Returns false for metadata/instant
// events that lack either field, and for a record whose read failed.
inline bool parse_ts_dur(json::Value& event, double& ts, double& dur) {
    EventScalars s;
    if (!parse_event_scalars(event, s) || !s.has_ts || !s.has_dur) return false;
    ts = s.ts;
    dur = s.dur;
    return true;
}

// Parse pid, tid, ts, dur of an event. Returns false for metadata/instant
// events that lack ts or dur.
inline bool parse_lane_ts_dur(dstd::StringView event, std::int64_t& pid,
                              std::int64_t& tid, double& ts, double& dur) {
    EventScalars s;
    if (!parse_event_scalars(event, s) || !s.has_ts || !s.has_dur) return false;
    ts = s.ts;
    dur = s.dur;
    pid = s.pid;
    tid = s.tid;
    return true;
}

// Containment depth per event/block, stable across zoom because an event's
// ancestors are always longer and so survive any threshold that kept it. A
// block is nested only under big events that fully cover its bucket interval
// [ts, ts+threshold]; a bucket-sized sibling that merely overlaps it is not an
// ancestor, so folded events stay on their sibling's row.
inline dstd::Vector<std::uint32_t> assign_view_depths(
    const dstd::Vector<dstd::String>& big, DensityMap& dens, double begin,
    double threshold) {
    dstd::Vector<std::uint32_t> depth(big.size(), 0);

    struct Item {
        double ts;          // interval start (big) or bucket left edge (block)
        double end;         // big end, or bucket right edge for a block query
        int big_idx;        // index into `big`, or -1 for a density query
        DensityAgg* block;  // set for a density query, null for a big event
        bool est = false;   // extrapolated aggregate event: peek, do not occupy
    };
    struct LaneKey {
        std::int64_t pid, tid;
        bool operator==(const LaneKey& o) const {
            return pid == o.pid && tid == o.tid;
        }
    };
    struct LaneHash {
        std::uint64_t operator()(const LaneKey& k) const noexcept {
            std::uint64_t h =
                dftracer::utils::dstd::hash::FNV1A_OFFSET_BASIS_LEGACY;
            h = (h ^ static_cast<std::uint64_t>(k.pid)) *
                dftracer::utils::dstd::hash::FNV1A_PRIME;
            h = (h ^ static_cast<std::uint64_t>(k.tid)) *
                dftracer::utils::dstd::hash::FNV1A_PRIME;
            return h;
        }
    };
    dstd::HashMap<LaneKey, dstd::Vector<Item>, LaneHash> lanes;

    for (std::size_t i = 0; i < big.size(); ++i) {
        std::int64_t pid = 0, tid = 0;
        double ts = 0, dur = 0;
        if (!parse_lane_ts_dur(big[i], pid, tid, ts, dur)) continue;
        double end = ts + (dur > 0 ? dur : 0);
        bool est = big[i].find("\"est\":true") != dstd::String::npos;
        lanes[LaneKey{pid, tid}].push_back(
            Item{ts, end, static_cast<int>(i), nullptr, est});
    }
    if (threshold > 0) {
        for (auto& kv : dens) {
            double lo = begin + static_cast<double>(kv.first.col) * threshold;
            lanes[LaneKey{kv.first.pid, kv.first.tid}].push_back(
                Item{lo, lo + threshold, -1, &kv.second});
        }
    }

    for (auto& kv : lanes) {
        auto& items = kv.second;
        // Opens sort before queries at equal ts so a block sitting exactly at a
        // parent's start counts that parent as an ancestor.
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.ts != b.ts) return a.ts < b.ts;
            return (a.block == nullptr) && (b.block != nullptr);
        });
        // Greedy lowest-free-row packing: staggered overlapping spans reuse
        // rows (real concurrency) instead of one row each, while nested events
        // still cascade.
        dstd::Vector<double> row_end;  // end time occupying each row
        for (auto& it : items) {
            if (it.block || it.est) {
                // Peek at the lowest row free at this time without occupying
                // it: aggregated/density marks sit right under the active call
                // stack and never add rows of their own.
                std::size_t r = 0;
                while (r < row_end.size() && row_end[r] > it.end) ++r;
                if (it.block)
                    it.block->depth = static_cast<std::uint32_t>(r);
                else
                    depth[static_cast<std::size_t>(it.big_idx)] =
                        static_cast<std::uint32_t>(r);
            } else {
                std::size_t r = 0;
                while (r < row_end.size() && row_end[r] > it.ts) ++r;
                if (r == row_end.size())
                    row_end.push_back(it.end);
                else
                    row_end[r] = it.end;
                depth[static_cast<std::size_t>(it.big_idx)] =
                    static_cast<std::uint32_t>(r);
            }
        }
    }
    return depth;
}

// Give each ph="C" counter series its own row, stacked above the events already
// occupying its (pid, tid) lane. assign_view_depths cannot: it treats density
// blocks as non-row-reserving (one per pid/tid/col), but a lane can hold many
// counter series at the same column, and a counter can share a real thread
// lane. Stable row order by series name so rows do not swap between
// requests/zooms.
inline void assign_counter_depths(const dstd::Vector<dstd::String>& big,
                                  const dstd::Vector<std::uint32_t>& big_depth,
                                  DensityMap& dens) {
    auto lane_of = [](std::int64_t pid, std::int64_t tid) {
        return (pid << 20) ^ tid;
    };
    dstd::HashMap<std::int64_t, std::uint32_t> lane_max;
    auto bump = [&](std::int64_t lane, std::uint32_t depth) {
        auto& m = lane_max[lane];
        if (depth + 1 > m) m = depth + 1;
    };
    for (std::size_t i = 0; i < big.size(); ++i) {
        std::int64_t pid = 0, tid = 0;
        double ts = 0, dur = 0;
        if (!parse_lane_ts_dur(big[i], pid, tid, ts, dur)) continue;
        bump(lane_of(pid, tid), i < big_depth.size() ? big_depth[i] : 0);
    }
    for (const auto& [k, a] : dens)
        if (!a.counter) bump(lane_of(k.pid, k.tid), a.depth);

    dstd::Map<std::int64_t, dstd::Map<dstd::String, std::uint32_t>> rows;
    for (const auto& [k, a] : dens)
        if (a.counter) rows[lane_of(k.pid, k.tid)][k.group];
    for (auto& [lane, groups] : rows) {
        auto it = lane_max.find(lane);
        std::uint32_t d = it == lane_max.end() ? 0 : it->second;
        for (auto& [g, depth] : groups) depth = d++;
    }
    for (auto& [k, a] : dens)
        if (a.counter) a.depth = rows[lane_of(k.pid, k.tid)][k.group];
}

// --- Counters (bandwidth / IOPS over time) ---
// Per-bucket read/write bytes and I/O op counts, aggregated from POSIX/STDIO/IO
// events (bytes come from args.ret).
struct CounterAcc {
    dstd::Vector<double> read_bytes;
    dstd::Vector<double> write_bytes;
    dstd::Vector<double> ops;
    void init(std::size_t n) {
        read_bytes.assign(n, 0.0);
        write_bytes.assign(n, 0.0);
        ops.assign(n, 0.0);
    }
    void merge_from(const CounterAcc& o) {
        for (std::size_t i = 0; i < ops.size(); ++i) {
            read_bytes[i] += o.read_bytes[i];
            write_bytes[i] += o.write_bytes[i];
            ops[i] += o.ops[i];
        }
    }
};

inline void fold_counter(dstd::StringView event, double begin, double bucket_us,
                         std::size_t buckets, CounterAcc& acc) {
    thread_local dftracer::utils::json::RecordParser parser;
    thread_local dstd::String buf;
    buf.assign(event);
    auto res = parse_string(parser, buf);
    if (res.error()) res = json::parse_refused(parser, event);
    if (res.error()) return;
    auto root = res.value_unsafe();

    EventScalars s;
    if (!parse_event_scalars(root, s) || !s.has_ts) return;
    if (s.cat != "POSIX" && s.cat != "STDIO" && s.cat != "IO") return;

    long col = static_cast<long>((s.ts - begin) / bucket_us);
    if (col < 0 || col >= static_cast<long>(buckets)) return;

    acc.ops[col] += 1.0;

    double bytes = 0;
    auto args = root["args"];
    if (!args.error() && args.is_object()) {
        auto ret = args["ret"];
        if (!ret.error()) bytes = json_number(ret.value_unsafe());
    }
    if (bytes <= 0) return;
    if (s.name.find("write") != dstd::StringView::npos)
        acc.write_bytes[col] += bytes;
    else if (s.name.find("read") != dstd::StringView::npos)
        acc.read_bytes[col] += bytes;
}

// --- Real ph="C" counters as aggregated density blocks ----------------------
// A counter event carries one or more numeric args; each (name, arg-key) is a
// series. Aggregation is aggregation of normal events, so a counter folds into
// the SAME density map: one block per (series, pid, tid, column) spanning the
// bucket. The series identity lives in the density key's `group` so distinct
// counters at the same pid/tid/column stay separate blocks (and lane/stack like
// normal events). value_sum/count gives the mean reading for hover.

// Add one counter sample of `value` for `series` (a "name.arg" identity) at
// (pid, tid, col) into the density map.
inline void add_counter_bucket(DensityMap& dens, std::int64_t pid,
                               std::int64_t tid, std::int64_t col,
                               dstd::StringView series, double value_sum,
                               std::uint32_t count) {
    if (count == 0) return;
    DensityKey k{pid, tid, col, dstd::String(series)};
    auto it = dens.find(k);
    if (it == dens.end()) it = dens.emplace(std::move(k), DensityAgg{}).first;
    auto& a = it->second;
    a.count += count;
    a.value_sum += value_sum;
    a.counter = true;
    if (a.name.empty()) a.name.assign(series);
}

// Fold one ph="C" event into the density map at its column, one block per
// numeric arg. Reads the event the partition already holds. Adds nothing when
// a read hit a syntax error.
inline void fold_counter_density(json::Value& root, double begin,
                                 double threshold, std::size_t ncols,
                                 DensityMap& dens) {
    if (threshold <= 0 || ncols == 0) return;
    EventScalars s;
    if (!parse_event_scalars(root, s) || !s.has_ts) return;
    long col = static_cast<long>((s.ts - begin) / threshold);
    if (col < 0 || col >= static_cast<long>(ncols)) return;

    auto args = root.find("args");
    if (!args || args->type() != json::Value::Type::Object) return;
    dstd::Vector<dstd::Pair<dstd::String, double>> samples;
    args->for_each_member([&](dstd::StringView key, json::Value v) {
        double val = 0;
        switch (v.type()) {
            case json::Value::Type::Int:
            case json::Value::Type::Double:
                val = value_number(v);
                break;
            default:
                return;  // non-numeric arg: not a counter sample
        }
        // Mirror the aggregator's track_default_args arg selection so we match
        // it exactly: skip metadata (hhash/fhash/dur/ret/offset/dftu_cnt) and
        // the pre-aggregated *_sum/_min/_max fields an already-aggregated trace
        // carries. Keeps junk lanes (dftu_cnt, ts, ...) out.
        if (index::schemas::dft::agg::is_reserved_arg(key) ||
            index::schemas::dft::agg::is_preagg_suffix(key))
            return;
        dstd::String series(s.name);
        series.push_back('.');
        series.append(key);
        samples.emplace_back(std::move(series), val);
    });
    if (json::detail::ValueAccess::broken(root)) return;
    for (const auto& [series, val] : samples)
        add_counter_bucket(dens, s.pid, s.tid, col, series, val, 1);
}

// A ph=3 SELECTIVE-aggregation record kept whole (so selection keeps every
// arg), plus the fields needed to place it: lane and name for a stable row, ts
// for window inference.
struct AggRec {
    dstd::String raw;
    double ts;  // window start, native time units
    std::int64_t pid;
    std::int64_t tid;
    dstd::String name;
    dstd::String cat;
    std::uint32_t count;  // dftu_cnt
    double total;         // dur_sum, native units
};

inline bool collect_aggregated(json::Value& root, dstd::StringView raw,
                               dstd::Vector<AggRec>& out) {
    EventScalars s;
    if (!parse_event_scalars(root, s) || !s.has_ts) return false;

    std::uint32_t count = 1;
    double total = 0;
    auto args = root.find("args");
    if (args && args->type() == json::Value::Type::Object) {
        bool has_cnt = false, has_dur_sum = false, has_dur = false;
        double cnt = 0, dur_sum = 0, dur = 0;
        args->for_each_member([&](dstd::StringView key, json::Value v) {
            if (key == "dftu_cnt") {
                if (has_cnt) return;
                cnt = value_number(v);
                has_cnt = true;
            } else if (key == "dur_sum") {
                if (has_dur_sum) return;
                dur_sum = value_number(v);
                has_dur_sum = true;
            } else if (key == "dur") {
                if (has_dur) return;
                dur = value_number(v);
                has_dur = true;
            }
        });
        if (has_cnt)
            count = cnt > 0 && std::isfinite(cnt)
                        ? static_cast<std::uint32_t>(cnt)
                        : 1;
        if (has_dur_sum)
            total = dur_sum;
        else if (has_dur)
            total = dur;
    }
    if (json::detail::ValueAccess::broken(root)) return false;
    out.push_back(AggRec{dstd::String(raw), s.ts, s.pid, s.tid,
                         dstd::String(s.name), dstd::String(s.cat), count,
                         total});
    return true;
}

// Split an aggregate into uniformly-spaced synthetic events that fall inside
// [begin, end], emitting each as a complete (ph=1) event so it nests by
// containment like a real event. Positions are estimated (marked "est":true),
// not real timings. Bounded by `cap` per aggregate: denser aggregates subsample
// so a 5s window of millions never floods the response.
inline void extrapolate_aggregate(const AggRec& r, double interval,
                                  double begin, double end, std::size_t cap,
                                  dstd::Vector<dstd::String>& big,
                                  dstd::Vector<double>& big_dur) {
    if (r.count == 0 || interval <= 0) return;
    double spacing = interval / static_cast<double>(r.count);
    if (spacing <= 0) return;
    double ev_dur = r.total / static_cast<double>(r.count);

    std::int64_t i_lo =
        static_cast<std::int64_t>(std::ceil((begin - r.ts) / spacing));
    std::int64_t i_hi =
        static_cast<std::int64_t>(std::floor((end - r.ts) / spacing));
    if (i_lo < 0) i_lo = 0;
    if (i_hi > static_cast<std::int64_t>(r.count) - 1)
        i_hi = static_cast<std::int64_t>(r.count) - 1;
    if (i_hi < i_lo) return;

    std::int64_t visible = i_hi - i_lo + 1;
    std::int64_t step = 1;
    if (cap > 0 && visible > static_cast<std::int64_t>(cap))
        step = (visible + static_cast<std::int64_t>(cap) - 1) /
               static_cast<std::int64_t>(cap);

    // Each synthetic event is the aggregate's own record with ts moved to the
    // estimated position and a mean dur added, so selection keeps every arg
    // (dftu_cnt, dur_sum, tag_min, ...). The record's ts is its window start;
    // we rewrite that one occurrence.
    const dstd::String ts_key =
        "\"ts\":" + dstd::to_string(static_cast<long long>(r.ts));
    const bool has_dur = std::isfinite(ev_dur);
    const dstd::String tail =
        has_dur ? ",\"dur\":" + dstd::to_string(ev_dur) + ",\"est\":true}"
                : dstd::String(",\"est\":true}");
    for (std::int64_t i = i_lo; i <= i_hi; i += step) {
        double ts_i = r.ts + static_cast<double>(i) * spacing;
        dstd::String e = r.raw;
        auto pos = e.find(ts_key);
        if (pos != dstd::String::npos)
            e.replace(
                pos, ts_key.size(),
                "\"ts\":" + dstd::to_string(static_cast<long long>(ts_i)));
        if (e.empty() || e.back() != '}') continue;
        e.pop_back();
        e += tail;
        big.push_back(std::move(e));
        big_dur.push_back(has_dur ? ev_dur : 0.0);
    }
}

// Smallest positive gap between distinct aggregate (ph=3) timestamps. Records
// are emitted on trace_interval_ms boundaries, so this recovers that window.
// Returns 0 when fewer than two distinct timestamps are present.
inline double infer_agg_interval(dstd::Vector<double> ts) {
    if (ts.size() < 2) return 0;
    std::sort(ts.begin(), ts.end());
    double best = 0;
    for (std::size_t i = 1; i < ts.size(); ++i) {
        double d = ts[i] - ts[i - 1];
        if (d > 0 && (best == 0 || d < best)) best = d;
    }
    return best;
}

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_VIZ_DENSITY_H
