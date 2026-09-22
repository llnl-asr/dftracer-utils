#include <dftracer/utils/core/common/hash/constants.h>
#include <dftracer/utils/dataframe/containment.h>
#include <dftracer/utils/dataframe/flame_arena.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/trace/views/containment_fold.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace dftracer::utils::trace::views::detail {

namespace df = dftracer::utils::dataframe;
using df::Series;

namespace {

inline std::uint64_t mix64(std::uint64_t x) {
    x ^= x >> 30;
    x *= dftracer::utils::hash::SPLITMIX64_MUL1;
    x ^= x >> 27;
    x *= dftracer::utils::hash::SPLITMIX64_MUL2;
    x ^= x >> 31;
    return x;
}

FieldRef classify(dftracer::utils::StringIntern& intern, const std::string& f,
                  bool& needs_args, std::vector<std::string>& nested) {
    if (f == "pid") return {FieldRef::Kind::Pid};
    if (f == "tid") return {FieldRef::Kind::Tid};
    if (f == "ts") return {FieldRef::Kind::Ts};
    if (f == "dur") return {FieldRef::Kind::Dur};
    if (f == "name") return {FieldRef::Kind::Name};
    if (f == "cat") return {FieldRef::Kind::Cat};
    if (is_nested_path(f))
        nested.push_back(f);
    else
        needs_args = true;
    return {FieldRef::Kind::Captured, intern.get_or_insert(f)};
}

const FoldEvent::ArgValue* find_arg(const FoldEvent& ev, std::uint32_t id) {
    for (const auto& [k, v] : ev.args)
        if (k == id) return &v;
    for (const auto& [k, v] : ev.top_fields)
        if (k == id) return &v;
    return nullptr;
}

std::int64_t field_i64(const FoldEvent& ev, const FieldRef& f) {
    switch (f.kind) {
        case FieldRef::Kind::Pid:
            return static_cast<std::int64_t>(ev.pid);
        case FieldRef::Kind::Tid:
            return static_cast<std::int64_t>(ev.tid);
        case FieldRef::Kind::Ts:
            return static_cast<std::int64_t>(ev.ts);
        case FieldRef::Kind::Dur:
            return static_cast<std::int64_t>(ev.dur);
        case FieldRef::Kind::Name:
            return ev.name_id;
        case FieldRef::Kind::Cat:
            return ev.cat_id;
        case FieldRef::Kind::Captured: {
            const FoldEvent::ArgValue* v = find_arg(ev, f.key_id);
            if (!v) return 0;
            if (const double* d = std::get_if<double>(v))
                return static_cast<std::int64_t>(*d);
            if (const std::int64_t* i = std::get_if<std::int64_t>(v)) return *i;
            return static_cast<std::int64_t>(std::get<std::uint32_t>(*v));
        }
    }
    return 0;
}

std::uint32_t field_str_id(const FoldEvent& ev, const FieldRef& f) {
    if (f.kind == FieldRef::Kind::Name) return ev.name_id;
    if (f.kind == FieldRef::Kind::Cat) return ev.cat_id;
    if (f.kind == FieldRef::Kind::Captured) {
        const FoldEvent::ArgValue* v = find_arg(ev, f.key_id);
        if (v)
            if (const std::uint32_t* s = std::get_if<std::uint32_t>(v))
                return *s;
    }
    return 0xFFFFFFFFu;
}

// The interned combined group value for this event ("<v0> / <v1> / ..."), or
// 0xFFFFFFFF when the spec has no group. A string-valued field contributes its
// resolved value; any other field its integer value.
std::uint32_t group_id_of(const FoldEvent& ev, const ContainmentSpec& spec,
                          dftracer::utils::StringIntern& intern) {
    if (spec.group_fields.empty()) return 0xFFFFFFFFu;
    std::string key;
    for (std::size_t i = 0; i < spec.group_fields.size(); ++i) {
        if (i) key += " / ";
        const FieldRef& f = spec.group_fields[i];
        const std::uint32_t sid = field_str_id(ev, f);
        if (sid != 0xFFFFFFFFu)
            key.append(intern.resolve(sid));
        else
            key.append(std::to_string(field_i64(ev, f)));
    }
    return intern.get_or_insert(key);
}

}  // namespace

std::vector<std::vector<std::int64_t>> sorted_lanes(
    const std::vector<ContainmentRow>& rows) {
    auto start = [&](std::int64_t i) {
        return rows[static_cast<std::size_t>(i)].start;
    };
    auto end = [&](std::int64_t i) {
        const ContainmentRow& r = rows[static_cast<std::size_t>(i)];
        return r.start + r.dur;
    };
    std::unordered_map<std::uint64_t, std::vector<std::int64_t>> lane_map;
    const std::int64_t n = static_cast<std::int64_t>(rows.size());
    for (std::int64_t i = 0; i < n; ++i)
        lane_map[rows[static_cast<std::size_t>(i)].lane].push_back(i);
    std::vector<std::vector<std::int64_t>> lanes;
    lanes.reserve(lane_map.size());
    for (auto& [key, idx] : lane_map) {
        (void)key;
        lanes.push_back(std::move(idx));
    }
    df::parallel_for(static_cast<std::int64_t>(lanes.size()), 1,
                     [&](std::int64_t lb, std::int64_t le) {
                         for (std::int64_t li = lb; li < le; ++li)
                             std::stable_sort(
                                 lanes[static_cast<std::size_t>(li)].begin(),
                                 lanes[static_cast<std::size_t>(li)].end(),
                                 [&](std::int64_t a, std::int64_t c) {
                                     if (start(a) != start(c))
                                         return start(a) < start(c);
                                     return end(a) > end(c);
                                 });
                     });
    return lanes;
}

ContainmentSpec make_containment_spec(dftracer::utils::StringIntern& intern,
                                      const std::vector<std::string>& partition,
                                      const std::string& start_field,
                                      const std::string& dur_field,
                                      const std::string& name_field,
                                      const std::vector<std::string>& group) {
    ContainmentSpec s;
    for (const std::string& f : partition)
        s.lane_fields.push_back(
            classify(intern, f, s.needs_args, s.nested_captures));
    for (const std::string& f : group)
        s.group_fields.push_back(
            classify(intern, f, s.needs_args, s.nested_captures));
    s.start_ref =
        classify(intern, start_field, s.needs_args, s.nested_captures);
    s.dur_ref = classify(intern, dur_field, s.needs_args, s.nested_captures);
    s.name_ref = classify(intern, name_field, s.needs_args, s.nested_captures);
    return s;
}

bool containment_row(const FoldEvent& ev, const ContainmentSpec& spec,
                     dftracer::utils::StringIntern& intern,
                     ContainmentRow& out) {
    if (ev.phase == RecordPhase::METADATA || ev.phase == RecordPhase::UNKNOWN)
        return false;
    if (spec.dur_ref.kind == FieldRef::Kind::Dur && !ev.has_dur) return false;
    std::uint64_t lane = dftracer::utils::hash::FNV1A_OFFSET_BASIS_LEGACY;
    for (const FieldRef& f : spec.lane_fields)
        lane = mix64(lane ^ static_cast<std::uint64_t>(field_i64(ev, f)));
    out = ContainmentRow{lane,
                         static_cast<std::int64_t>(ev.pid),
                         static_cast<std::int64_t>(ev.tid),
                         field_i64(ev, spec.start_ref),
                         field_i64(ev, spec.dur_ref),
                         field_str_id(ev, spec.name_ref),
                         group_id_of(ev, spec, intern)};
    return true;
}

ContainmentFold::ContainmentFold(dftracer::utils::StringIntern& intern,
                                 std::vector<std::string> partition,
                                 std::string start_field, std::string dur_field,
                                 std::string name_field, double time_scale,
                                 std::vector<std::string> group)
    : intern_(&intern),
      spec_(make_containment_spec(intern, partition, start_field, dur_field,
                                  name_field, group)),
      time_scale_(time_scale) {}

std::unique_ptr<Fold> ContainmentFold::slice() const {
    auto s = std::make_unique<ContainmentFold>(*this);
    s->rows_.clear();
    return s;
}

void ContainmentFold::step(const FoldBatch& batch) {
    ContainmentRow r;
    for (const FoldEvent& ev : batch.events)
        if (containment_row(ev, spec_, *intern_, r)) rows_.push_back(r);
}

void ContainmentFold::merge(Fold& other) {
    auto& o = static_cast<ContainmentFold&>(other);
    rows_.insert(rows_.end(), std::make_move_iterator(o.rows_.begin()),
                 std::make_move_iterator(o.rows_.end()));
    o.rows_.clear();
}

dataframe::DataFrame build_call_tree(
    const std::vector<ContainmentRow>& rows,
    const std::vector<std::vector<std::int64_t>>& lanes,
    const dftracer::utils::StringIntern& intern, double time_scale) {
    const std::int64_t n = static_cast<std::int64_t>(rows.size());
    const std::int64_t nlanes = static_cast<std::int64_t>(lanes.size());
    auto start = [&](std::int64_t i) {
        return rows[static_cast<std::size_t>(i)].start;
    };
    auto end = [&](std::int64_t i) {
        const ContainmentRow& r = rows[static_cast<std::size_t>(i)];
        return r.start + r.dur;
    };

    std::vector<std::int64_t> level(static_cast<std::size_t>(n), 0);
    std::vector<std::int64_t> parent(static_cast<std::size_t>(n), -1);
    df::parallel_for(nlanes, 1, [&](std::int64_t lb, std::int64_t le) {
        std::vector<std::pair<std::int64_t, std::int64_t>> stack;
        for (std::int64_t li = lb; li < le; ++li) {
            const std::vector<std::int64_t>& lane =
                lanes[static_cast<std::size_t>(li)];
            df::containment_walk(
                static_cast<std::int64_t>(lane.size()),
                [&](std::int64_t k) { return start(lane[k]); },
                [&](std::int64_t k) { return end(lane[k]); }, std::int64_t(-1),
                [&](std::int64_t k, std::int64_t lv,
                    std::int64_t par) -> std::int64_t {
                    const std::int64_t r = lane[k];
                    level[static_cast<std::size_t>(r)] = lv;
                    parent[static_cast<std::size_t>(r)] = par;
                    return r;
                },
                stack);
        }
    });

    std::vector<std::int64_t> pid(static_cast<std::size_t>(n));
    std::vector<std::int64_t> tid(static_cast<std::size_t>(n));
    std::vector<std::int64_t> ts(static_cast<std::size_t>(n));
    std::vector<std::int64_t> dur(static_cast<std::size_t>(n));
    std::vector<std::string> names(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        const ContainmentRow& r = rows[static_cast<std::size_t>(i)];
        pid[static_cast<std::size_t>(i)] = r.pid;
        tid[static_cast<std::size_t>(i)] = r.tid;
        ts[static_cast<std::size_t>(i)] = static_cast<std::int64_t>(
            static_cast<double>(r.start) * time_scale);
        dur[static_cast<std::size_t>(i)] =
            static_cast<std::int64_t>(static_cast<double>(r.dur) * time_scale);
        names[static_cast<std::size_t>(i)] =
            r.name_id == 0xFFFFFFFFu ? std::string()
                                     : std::string(intern.resolve(r.name_id));
    }
    dataframe::DataFrame out;
    out.names = {"pid", "tid", "ts", "dur", "name", "level", "parent_id"};
    out.columns.push_back(Series::flat_i64(pid.data(), n));
    out.columns.push_back(Series::flat_i64(tid.data(), n));
    out.columns.push_back(Series::flat_i64(ts.data(), n));
    out.columns.push_back(Series::flat_i64(dur.data(), n));
    out.columns.push_back(Series::strings(names));
    out.columns.push_back(Series::flat_i64(level.data(), n));
    out.columns.push_back(Series::flat_i64(parent.data(), n));
    return out;
}

static std::vector<df::FlameNode> fold_flame_arena(
    const std::vector<ContainmentRow>& rows,
    const std::vector<std::vector<std::int64_t>>& lanes,
    const dftracer::utils::StringIntern& intern, double time_scale) {
    const std::int64_t nlanes = static_cast<std::int64_t>(lanes.size());
    auto start = [&](std::int64_t i) {
        return rows[static_cast<std::size_t>(i)].start;
    };
    auto end = [&](std::int64_t i) {
        const ContainmentRow& r = rows[static_cast<std::size_t>(i)];
        return r.start + r.dur;
    };
    auto name_of = [&](std::uint32_t id) -> std::string_view {
        return id == 0xFFFFFFFFu ? std::string_view{} : intern.resolve(id);
    };

    std::vector<std::vector<df::FlameNode>> arenas(
        static_cast<std::size_t>(nlanes));
    df::parallel_for(nlanes, 1, [&](std::int64_t lb, std::int64_t le) {
        std::vector<std::pair<std::int64_t, std::uint32_t>> stack;
        for (std::int64_t li = lb; li < le; ++li) {
            const std::vector<std::int64_t>& lane =
                lanes[static_cast<std::size_t>(li)];
            std::vector<df::FlameNode>& arena =
                arenas[static_cast<std::size_t>(li)];
            arena.emplace_back();
            arena[0].name = "all";
            df::containment_walk(
                static_cast<std::int64_t>(lane.size()),
                [&](std::int64_t k) { return start(lane[k]); },
                [&](std::int64_t k) { return end(lane[k]); },
                static_cast<std::uint32_t>(0),
                [&](std::int64_t k, std::int64_t, std::uint32_t par) {
                    const ContainmentRow& r =
                        rows[static_cast<std::size_t>(lane[k])];
                    return df::fold_flame_node(
                        arena, name_of(r.name_id),
                        static_cast<double>(r.dur) * time_scale, par);
                },
                stack);
        }
    });

    std::vector<df::FlameNode> merged;
    merged.emplace_back();
    merged[0].name = "all";
    // A group key (any field(s), e.g. pid/cat/hostname) roots each lane under a
    // synthetic node named by the lane's group value - it is lane-constant, so
    // it is read from the lane's first row. With no group key every lane folds
    // together by name path under the single "all" root.
    ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> group_of;
    for (std::size_t li = 0; li < arenas.size(); ++li) {
        std::vector<df::FlameNode>& a = arenas[li];
        if (a.empty() || lanes[li].empty()) continue;
        const std::uint32_t gid =
            rows[static_cast<std::size_t>(lanes[li][0])].group_id;
        std::uint32_t target = 0;
        if (gid != 0xFFFFFFFFu) {
            auto it = group_of.find(gid);
            if (it == group_of.end()) {
                target = static_cast<std::uint32_t>(merged.size());
                merged.emplace_back();
                merged[target].name.assign(intern.resolve(gid));
                merged[0].children.push_back(target);
                merged[0].kids.emplace(merged[target].name, target);
                group_of.emplace(gid, target);
            } else {
                target = it->second;
            }
        }
        df::merge_flame_arena(merged, target, a, 0);
    }
    // Roll up each synthetic group node's stats from its children (self stays
    // 0: a group node holds no events of its own), so every consumer sees a
    // consistent arena whether or not a group key was set.
    for (std::uint32_t g : merged[0].children) {
        if (merged[g].total != 0 || merged[g].children.empty()) continue;
        for (std::uint32_t c : merged[g].children) {
            merged[g].total += merged[c].total;
            merged[g].count += merged[c].count;
        }
    }
    return merged;
}

// Root rollup (sum top-level totals) + emit the node frame.
static dataframe::DataFrame flame_arena_to_frame(
    std::vector<df::FlameNode> merged) {
    if (merged.empty()) {
        merged.emplace_back();
        merged[0].name = "all";
    }
    for (std::uint32_t c : merged[0].children) {
        merged[0].total += merged[c].total;
        merged[0].count += merged[c].count;
    }

    const std::int64_t nn = static_cast<std::int64_t>(merged.size());
    std::vector<std::int64_t> node_id(static_cast<std::size_t>(nn));
    std::vector<std::int64_t> par(static_cast<std::size_t>(nn), -1);
    std::vector<std::int64_t> level(static_cast<std::size_t>(nn), 0);
    std::vector<std::int64_t> count(static_cast<std::size_t>(nn));
    std::vector<double> total(static_cast<std::size_t>(nn));
    std::vector<double> self(static_cast<std::size_t>(nn));
    std::vector<std::string> names(static_cast<std::size_t>(nn));
    for (std::int64_t i = 0; i < nn; ++i) {
        const df::FlameNode& nd = merged[static_cast<std::size_t>(i)];
        node_id[static_cast<std::size_t>(i)] = i;
        names[static_cast<std::size_t>(i)] = nd.name;
        total[static_cast<std::size_t>(i)] = nd.total;
        self[static_cast<std::size_t>(i)] = nd.self < 0 ? 0.0 : nd.self;
        count[static_cast<std::size_t>(i)] =
            static_cast<std::int64_t>(nd.count);
        for (std::uint32_t c : nd.children) {
            par[c] = i;
            level[c] = level[static_cast<std::size_t>(i)] + 1;
        }
    }
    dataframe::DataFrame out;
    out.names = {"node_id", "parent", "name", "level",
                 "total",   "self",   "count"};
    out.columns.push_back(Series::flat_i64(node_id.data(), nn));
    out.columns.push_back(Series::flat_i64(par.data(), nn));
    out.columns.push_back(Series::strings(names));
    out.columns.push_back(Series::flat_i64(level.data(), nn));
    out.columns.push_back(Series::flat_f64(total.data(), nn));
    out.columns.push_back(Series::flat_f64(self.data(), nn));
    out.columns.push_back(Series::flat_i64(count.data(), nn));
    return out;
}

dataframe::DataFrame build_flamegraph(
    const std::vector<ContainmentRow>& rows,
    const std::vector<std::vector<std::int64_t>>& lanes,
    const dftracer::utils::StringIntern& intern, double time_scale) {
    return flame_arena_to_frame(
        fold_flame_arena(rows, lanes, intern, time_scale));
}

std::string flamegraph_partial(const std::vector<ContainmentRow>& rows,
                               const dftracer::utils::StringIntern& intern,
                               double time_scale) {
    std::vector<std::uint8_t> b = df::serialize_flame_arena(
        fold_flame_arena(rows, sorted_lanes(rows), intern, time_scale));
    return std::string(b.begin(), b.end());
}

dataframe::DataFrame merge_flamegraph_partials(
    const std::vector<std::string_view>& partials) {
    std::vector<df::FlameNode> merged;
    merged.emplace_back();
    merged[0].name = "all";
    for (std::string_view p : partials) {
        std::vector<df::FlameNode> a = df::deserialize_flame_arena(
            reinterpret_cast<const std::uint8_t*>(p.data()), p.size());
        if (!a.empty()) df::merge_flame_arena(merged, 0, a, 0);
    }
    return flame_arena_to_frame(std::move(merged));
}

std::pair<dataframe::DataFrame, dataframe::DataFrame> build_containment_both(
    const std::vector<ContainmentRow>& rows,
    const dftracer::utils::StringIntern& intern, double time_scale) {
    std::vector<std::vector<std::int64_t>> lanes = sorted_lanes(rows);
    return {build_call_tree(rows, lanes, intern, time_scale),
            build_flamegraph(rows, lanes, intern, time_scale)};
}

}  // namespace dftracer::utils::trace::views::detail
