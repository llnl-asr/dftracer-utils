#ifndef DFTRACER_TESTS_GROUPMAP_ORACLE_H
#define DFTRACER_TESTS_GROUPMAP_ORACLE_H

// An independent, test-local reimplementation of the pre-convergence GroupMap
// aggregation fold (the AggAccum map + to_batch materializer that the dataframe
// engine replaced). It shares only the query-derived schema and the
// key/column-name/resolver helpers with production; the accumulation, merge,
// finalize and materialize logic is its own, so it is a genuine oracle for the
// engine aggregation path rather than a wrapper around it.

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/trace/aggregators/reserved_args.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/agg_fold.h>
#include <dftracer/utils/trace/views/aggfold.h>
#include <dftracer/utils/trace/views/event_source.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/utilities/common/statistics/ddsketch.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace gmoracle {

namespace detail = dftracer::utils::trace::views::detail;
namespace dataframe = dftracer::utils::dataframe;
using detail::AggSchema;
using detail::GROUP_SEP;
using detail::SET_SEP;
using detail::ViewPlan;
using dftracer::utils::StringIntern;
using dftracer::utils::dataframe::FieldNum;
using dftracer::utils::dataframe::FieldStat;
using dftracer::utils::dataframe::FieldStatDomain;
using dftracer::utils::trace::RecordPhase;
using dftracer::utils::trace::views::AggOp;
using dftracer::utils::trace::views::AggSpec;
using dftracer::utils::trace::views::GroupKey;
using dftracer::utils::trace::views::ViewDefinition;
namespace stats = dftracer::utils::utilities::common::statistics;

struct ArgMaxState {
    double by = 0;
    std::string repr;
    bool has = false;
};

struct AggAccum {
    std::vector<std::string> keys;
    std::uint64_t count = 0;
    std::vector<FieldStat> fields;
    std::vector<ArgMaxState> argmax;
    std::vector<stats::DDSketch> sketches;
    std::vector<std::set<std::string>> sets;
    std::map<std::string, FieldStat> dyn;
    std::map<std::string, stats::DDSketch> dyn_sketches;
    ankerl::unordered_dense::map<std::uint64_t, std::int64_t> occ_deltas;
    std::uint64_t occ_cell_us = 0;
    std::uint64_t occ_total = 0;
    std::uint64_t occ_ts = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t occ_te = 0;
};

using GroupMap = ankerl::unordered_dense::map<std::string, AggAccum>;

template <class Src>
void append_group_dim(std::string& out, const Src& src, const GroupKey& gk) {
    switch (gk.kind) {
        case GroupKey::Kind::Name:
            src.append_value(out, "name");
            break;
        case GroupKey::Kind::Cat: {
            const std::size_t start = out.size();
            src.append_value(out, "cat");
            detail::lower_ascii(out, start);
            break;
        }
        case GroupKey::Kind::Pid:
            src.append_value(out, "pid");
            break;
        case GroupKey::Kind::Tid:
            src.append_value(out, "tid");
            break;
        case GroupKey::Kind::Fhash:
        case GroupKey::Kind::FilePath:
        case GroupKey::Kind::FileName:
            src.append_arg(out, "fhash");
            break;
        case GroupKey::Kind::Hhash:
        case GroupKey::Kind::HostName:
            src.append_arg(out, "hhash");
            break;
        case GroupKey::Kind::IoCat: {
            char buf[8];
            char* p = dftracer::utils::to_chars_i64(
                buf, buf + sizeof(buf),
                static_cast<int>(dftracer::utils::trace::internal::io_category(
                    src.value("name"))));
            out.append(buf, static_cast<std::size_t>(p - buf));
            break;
        }
        case GroupKey::Kind::AccPat:
            out.push_back('0');
            break;
        case GroupKey::Kind::Rank:
            src.append_value(out, "pid");
            break;
        case GroupKey::Kind::Arg:
            src.append_arg(out, gk.arg);
            break;
        case GroupKey::Kind::Field:
            src.append_value(out, gk.arg);
            break;
        case GroupKey::Kind::Expr:
            // The oracle predates computed columns; no parity case uses one.
            break;
    }
}

template <class Src>
std::string group_dim_str(const Src& src, const GroupKey& gk) {
    std::string s;
    append_group_dim(s, src, gk);
    return s;
}

template <class Src>
void fold_numeric_args_t(AggAccum& a, const Src& src, bool want_sketch) {
    namespace agg = dftracer::utils::trace::aggregators;
    auto feed = [&](std::string_view key, double num) {
        a.dyn[std::string(key)].add(num);
        if (want_sketch) a.dyn_sketches[std::string(key)].add(num);
    };
    if (auto sz = detail::derived_size_t(src)) feed("size", *sz);
    src.for_each_numeric_arg([&](std::string_view key, double num) {
        if (agg::is_reserved_arg(key) || agg::is_preagg_suffix(key)) return;
        feed(key, num);
    });
}

template <class Src>
void fold_event_over(GroupMap& map, const Src& src, const ViewPlan& plan,
                     std::string& keybuf) {
    const AggSchema& sch = *plan.schema;
    keybuf.clear();
    std::int64_t bucket = 0;
    const bool has_bucket = plan.time_bucket_us > 0;
    if (has_bucket) {
        auto ts = detail::agg_field_t(src, "ts");
        if (!ts) return;
        const auto interval = static_cast<double>(plan.time_bucket_us);
        const auto w = static_cast<std::int64_t>(plan.time_bucket_us);
        const auto origin = static_cast<std::int64_t>(plan.bucket_origin_us);
        const double rel = *ts * plan.time_scale - static_cast<double>(origin);
        bucket =
            static_cast<std::int64_t>(std::floor(rel / interval)) * w + origin;
        char b[24];
        char* p = dftracer::utils::to_chars_i64(b, b + sizeof(b), bucket);
        keybuf.append(b, static_cast<std::size_t>(p - b));
        keybuf += GROUP_SEP;
    }
    for (const auto& gk : plan.group_by) {
        append_group_dim(keybuf, src, gk);
        keybuf += GROUP_SEP;
    }

    auto it = map.find(keybuf);
    if (it == map.end()) {
        AggAccum a;
        a.keys.reserve(plan.group_by.size() + (has_bucket ? 1 : 0));
        if (has_bucket) a.keys.push_back(std::to_string(bucket));
        for (const auto& gk : plan.group_by)
            a.keys.push_back(group_dim_str(src, gk));
        a.fields.resize(sch.fields.size());
        a.argmax.resize(sch.argmax_count);
        a.sketches.resize(sch.sketch_count);
        a.sets.resize(sch.set_count);
        it = map.emplace(keybuf, std::move(a)).first;
    }
    AggAccum& a = it->second;
    ++a.count;

    if (sch.want_occupancy) {
        auto ts = src.number("ts");
        auto dur = src.number("dur");
        if (ts && dur && *dur > 0) {
            std::uint64_t s = static_cast<std::uint64_t>(*ts);
            std::uint64_t e = s + static_cast<std::uint64_t>(*dur);
            const std::uint64_t cell = sch.occ_cell_us;
            if (cell) {
                s = s / cell * cell;
                e = (e + cell - 1) / cell * cell;
            }
            a.occ_cell_us = cell;
            a.occ_total += static_cast<std::uint64_t>(*dur);
            if (s < a.occ_ts) a.occ_ts = s;
            if (e > a.occ_te) a.occ_te = e;
            a.occ_deltas[s] += 1;
            a.occ_deltas[e] -= 1;
        }
    }

    for (std::size_t fi = 0; fi < sch.fields.size(); ++fi) {
        auto v = detail::agg_field_typed_t(src, sch.fields[fi]);
        if (!v) continue;
        const int sk = sch.field_sketch[fi];
        if (sch.field_scaled[fi] && plan.time_scale != 1.0) {
            const double x = v->as_double() * plan.time_scale;
            a.fields[fi].add(x);
            if (sk >= 0) a.sketches[static_cast<std::size_t>(sk)].add(x);
        } else {
            a.fields[fi].add(*v);
            if (sk >= 0)
                a.sketches[static_cast<std::size_t>(sk)].add(v->as_double());
        }
    }
    for (std::size_t i = 0; i < plan.agg.size(); ++i) {
        const int slot = sch.spec_argmax[i];
        if (slot < 0) continue;
        auto by = detail::agg_field_t(src, plan.agg[i].by);
        if (!by) continue;
        auto& am = a.argmax[slot];
        if (!am.has || *by > am.by) {
            am.by = *by;
            am.repr = src.value(plan.agg[i].field);
            am.has = true;
        }
    }
    for (std::size_t i = 0; i < plan.agg.size(); ++i) {
        const int slot = sch.spec_set[i];
        if (slot < 0) continue;
        std::string v = src.value(plan.agg[i].field);
        if (!v.empty()) a.sets[slot].insert(std::move(v));
    }
    if (plan.auto_numeric_metrics) fold_numeric_args_t(a, src, sch.dyn_sketch);
}

inline void merge_accum(AggAccum& da, const AggAccum& sa,
                        const ViewPlan& plan) {
    const AggSchema& sch = *plan.schema;
    if (da.count == 0) {
        da.keys = sa.keys;
        da.fields.resize(sch.fields.size());
        da.argmax.resize(sch.argmax_count);
        da.sketches.resize(sch.sketch_count);
        da.sets.resize(sch.set_count);
    }
    da.count += sa.count;
    for (std::size_t i = 0; i < da.fields.size() && i < sa.fields.size(); ++i)
        da.fields[i].merge(sa.fields[i]);
    for (std::size_t i = 0; i < da.argmax.size() && i < sa.argmax.size(); ++i) {
        const auto& s = sa.argmax[i];
        if (!s.has) continue;
        auto& d = da.argmax[i];
        if (!d.has || s.by > d.by) d = s;
    }
    for (std::size_t i = 0; i < da.sketches.size() && i < sa.sketches.size();
         ++i)
        da.sketches[i].merge(sa.sketches[i]);
    for (std::size_t i = 0; i < da.sets.size() && i < sa.sets.size(); ++i)
        da.sets[i].insert(sa.sets[i].begin(), sa.sets[i].end());
    for (const auto& [name, sm] : sa.dyn) da.dyn[name].merge(sm);
    for (const auto& [name, sk] : sa.dyn_sketches)
        da.dyn_sketches[name].merge(sk);
    if (!sa.occ_deltas.empty() || sa.occ_total) {
        da.occ_total += sa.occ_total;
        if (sa.occ_ts < da.occ_ts) da.occ_ts = sa.occ_ts;
        if (sa.occ_te > da.occ_te) da.occ_te = sa.occ_te;
        for (const auto& [t, dlt] : sa.occ_deltas) da.occ_deltas[t] += dlt;
        if (sa.occ_cell_us) da.occ_cell_us = sa.occ_cell_us;
    }
}

inline void merge_maps(GroupMap& dst, const GroupMap& src,
                       const ViewPlan& plan) {
    for (const auto& [k, sa] : src) merge_accum(dst[k], sa, plan);
}

// Name maps harvested from the trace's own FH/HH/PR metadata records, so the
// oracle resolves fhash/hhash/rank group keys without depending on the built
// index's name tables.
struct HarvestMaps {
    std::map<std::string, std::string> fpath;             // fhash -> file path
    std::map<std::string, std::string> host;              // hhash -> host name
    std::unordered_map<std::uint64_t, std::string> rank;  // pid -> rank
    std::string resolve(GroupKey::Kind kind, const std::string& v) const {
        if (kind == GroupKey::Kind::FilePath) {
            auto it = fpath.find(v);
            return it != fpath.end() ? it->second : v;
        }
        if (kind == GroupKey::Kind::FileName) {
            auto it = fpath.find(v);
            const std::string& p = it != fpath.end() ? it->second : v;
            const std::size_t slash = p.find_last_of('/');
            return slash == std::string::npos ? p : p.substr(slash + 1);
        }
        if (kind == GroupKey::Kind::HostName) {
            auto it = host.find(v);
            return it != host.end() ? it->second : v;
        }
        if (kind == GroupKey::Kind::Rank) {
            auto it = rank.find(static_cast<std::uint64_t>(std::stoull(v)));
            return it != rank.end() ? it->second : v;
        }
        return v;
    }
};

inline void resolve_group_keys(GroupMap& map, const ViewPlan& plan,
                               const HarvestMaps& maps) {
    const bool needs_resolve = std::any_of(
        plan.group_by.begin(), plan.group_by.end(), [](const auto& g) {
            return g.kind == GroupKey::Kind::FilePath ||
                   g.kind == GroupKey::Kind::FileName ||
                   g.kind == GroupKey::Kind::HostName ||
                   g.kind == GroupKey::Kind::Rank;
        });
    const bool any_transform = std::any_of(
        plan.group_by.begin(), plan.group_by.end(),
        [](const auto& g) { return g.transform != GroupKey::Transform::None; });
    if (!needs_resolve && !any_transform) return;
    const std::size_t off = plan.time_bucket_us > 0 ? 1 : 0;
    GroupMap out;
    std::string newkey;
    for (auto& [k, accum] : map) {
        (void)k;
        for (std::size_t j = 0; j < plan.group_by.size(); ++j) {
            const std::size_t idx = off + j;
            if (idx >= accum.keys.size()) continue;
            std::string v =
                maps.resolve(plan.group_by[j].kind, std::move(accum.keys[idx]));
            accum.keys[idx] =
                detail::apply_group_transform(plan.group_by[j], std::move(v));
        }
        newkey.clear();
        for (const auto& part : accum.keys) {
            newkey += part;
            newkey += GROUP_SEP;
        }
        merge_accum(out[newkey], accum, plan);
    }
    map = std::move(out);
}

struct OccSummary {
    std::uint64_t busy = 0;
    std::uint64_t active = 0;
    std::uint64_t total = 0;
    std::uint64_t span = 0;
};

inline OccSummary occupancy_summary(const AggAccum& a) {
    OccSummary o;
    o.total = a.occ_total;
    o.span = a.occ_te > a.occ_ts ? a.occ_te - a.occ_ts : 0;
    if (a.occ_deltas.empty()) return o;
    std::vector<std::pair<std::uint64_t, std::int64_t>> pts(
        a.occ_deltas.begin(), a.occ_deltas.end());
    std::sort(pts.begin(), pts.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    std::int64_t depth = 0;
    std::uint64_t last = 0, busy = 0, peak = 0;
    bool started = false;
    for (const auto& [t, dlt] : pts) {
        if (started && depth > 0) busy += t - last;
        depth += dlt;
        if (depth > 0 && static_cast<std::uint64_t>(depth) > peak)
            peak = static_cast<std::uint64_t>(depth);
        last = t;
        started = true;
    }
    o.busy = busy;
    o.active = peak;
    return o;
}

inline double finalize_value(const AggAccum& a, const ViewPlan& plan,
                             std::size_t i) {
    const AggSchema& sch = *plan.schema;
    const auto& spec = plan.agg[i];
    const int fi = sch.spec_field[i];
    const FieldStat* fs = fi >= 0 ? &a.fields[fi] : nullptr;
    const double N = static_cast<double>(a.count);
    switch (spec.op) {
        case AggOp::Count:
            return spec.field.empty() ? N
                                      : (fs ? static_cast<double>(fs->n) : 0.0);
        case AggOp::Sum:
            return fs ? fs->sum : 0.0;
        case AggOp::Min:
            return fs ? fs->min : 0.0;
        case AggOp::Max:
            return fs ? fs->max : 0.0;
        case AggOp::SumSq:
            return fs ? fs->sumsq : 0.0;
        case AggOp::Mean:
            return fs ? fs->mean() : 0.0;
        case AggOp::Var:
            return fs ? fs->variance(true) : 0.0;
        case AggOp::Std:
            return fs ? fs->stddev(true) : 0.0;
        case AggOp::Pct: {
            const int sk = fi >= 0 ? sch.field_sketch[fi] : -1;
            return sk >= 0 ? a.sketches[sk].quantile(spec.q) : 0.0;
        }
        case AggOp::Skew:
            return fs ? fs->skewness() : 0.0;
        case AggOp::Kurt:
            return fs ? fs->kurtosis() : 0.0;
        case AggOp::ArgMax:
        case AggOp::Hist:
        case AggOp::SetUnion:
            return 0.0;
        case AggOp::Busy:
            return static_cast<double>(occupancy_summary(a).busy);
        case AggOp::Concurrency: {
            const OccSummary o = occupancy_summary(a);
            return o.busy ? static_cast<double>(o.total) /
                                static_cast<double>(o.busy)
                          : 0.0;
        }
        case AggOp::Utilization: {
            const OccSummary o = occupancy_summary(a);
            return (o.busy && o.span) ? static_cast<double>(o.busy) /
                                            static_cast<double>(o.span)
                                      : 0.0;
        }
        case AggOp::Active:
            return static_cast<double>(occupancy_summary(a).active);
    }
    return 0.0;
}

inline FieldNum finalize_value_typed(const AggAccum& a, const ViewPlan& plan,
                                     std::size_t i) {
    const AggSchema& sch = *plan.schema;
    const auto& spec = plan.agg[i];
    const int fi = sch.spec_field[i];
    const FieldStat* fs = fi >= 0 ? &a.fields[fi] : nullptr;
    auto exact = [&](std::int64_t bits, double dbl) -> FieldNum {
        if (!fs) return FieldNum::of(dbl);
        switch (fs->domain) {
            case FieldStatDomain::I64:
                return FieldNum::of(bits);
            case FieldStatDomain::U64:
                return FieldNum::of(std::bit_cast<std::uint64_t>(bits));
            default:
                return FieldNum::of(dbl);
        }
    };
    switch (spec.op) {
        case AggOp::Count:
            return FieldNum::of(static_cast<std::int64_t>(
                spec.field.empty() ? a.count : (fs ? fs->n : 0)));
        case AggOp::Sum:
            return fs ? exact(fs->esum, fs->sum) : FieldNum::of(0.0);
        case AggOp::Min:
            return fs ? exact(fs->emin, fs->min) : FieldNum::of(0.0);
        case AggOp::Max:
            return fs ? exact(fs->emax, fs->max) : FieldNum::of(0.0);
        default:
            return FieldNum::of(finalize_value(a, plan, i));
    }
}

inline std::string finalize_set(const AggAccum& a, const ViewPlan& plan,
                                std::size_t i) {
    const int si = plan.schema->spec_set[i];
    if (si < 0 || static_cast<std::size_t>(si) >= a.sets.size()) return {};
    std::string out;
    for (const auto& v : a.sets[si]) {
        if (!out.empty()) out.push_back(SET_SEP);
        out += v;
    }
    return out;
}

inline std::vector<stats::HistogramBin> finalize_hist(const AggAccum& a,
                                                      const ViewPlan& plan,
                                                      std::size_t i) {
    const AggSchema& sch = *plan.schema;
    const int fi = sch.spec_field[i];
    const int sk = fi >= 0 ? sch.field_sketch[fi] : -1;
    if (sk < 0) return {};
    return a.sketches[sk].bins();
}

inline double reduce_dyn(const FieldStat& fs, AggOp op, double) {
    switch (op) {
        case AggOp::Count:
            return static_cast<double>(fs.n);
        case AggOp::Sum:
            return fs.sum;
        case AggOp::Min:
            return fs.n ? fs.min : 0.0;
        case AggOp::Max:
            return fs.n ? fs.max : 0.0;
        case AggOp::SumSq:
            return fs.sumsq;
        case AggOp::Mean:
            return fs.mean();
        case AggOp::Var:
            return fs.variance(true);
        case AggOp::Std:
            return fs.stddev(true);
        case AggOp::Skew:
            return fs.skewness();
        case AggOp::Kurt:
            return fs.kurtosis();
        default:
            return 0.0;
    }
}

inline dataframe::DataFrame to_batch(const GroupMap& map,
                                     const ViewPlan& plan) {
    std::vector<std::string> group_cols, value_cols, text_cols, hist_cols;
    if (plan.time_bucket_us > 0) group_cols.push_back("time_bucket");
    for (const auto& gk : plan.group_by)
        group_cols.push_back(detail::group_col_name(gk));
    const bool count_col = plan.agg.empty();
    if (count_col) {
        value_cols.push_back("count");
    } else {
        for (const auto& spec : plan.agg) {
            if (spec.op == AggOp::ArgMax || spec.op == AggOp::SetUnion)
                text_cols.push_back(detail::agg_col_name(spec));
            else if (spec.op == AggOp::Hist)
                hist_cols.push_back(detail::agg_col_name(spec));
            else
                value_cols.push_back(detail::agg_col_name(spec));
        }
    }
    struct DynCol {
        std::string key;
        AggSpec spec;
    };
    std::vector<DynCol> dyn_cols;
    if (plan.auto_numeric_metrics) {
        std::set<std::string> names;
        for (const auto& [k, a] : map) {
            (void)k;
            for (const auto& [name, m] : a.dyn) names.insert(name);
        }
        if (plan.numeric_arg_aggs.empty()) {
            for (const auto& n : names) {
                dyn_cols.push_back({n, AggSpec(AggOp::Mean)});
                value_cols.push_back(n);
            }
        } else {
            for (const auto& n : names)
                for (const auto& spec : plan.numeric_arg_aggs) {
                    dyn_cols.push_back({n, spec});
                    value_cols.push_back(detail::dyn_col_name(spec, n));
                }
        }
    }
    bool occ_cell_col = false;
    for (const auto& spec : plan.agg)
        if (spec.op == AggOp::Busy || spec.op == AggOp::Concurrency ||
            spec.op == AggOp::Utilization) {
            occ_cell_col = true;
            break;
        }
    if (occ_cell_col) value_cols.push_back("busy_cell_us");

    const std::size_t ng = map.size();
    std::vector<std::vector<std::string>> gk(group_cols.size());
    for (auto& v : gk) v.reserve(ng);
    std::vector<std::vector<FieldNum>> vcells(value_cols.size());
    for (auto& v : vcells) v.reserve(ng);
    std::vector<std::vector<std::string>> tvals(text_cols.size());
    for (auto& v : tvals) v.reserve(ng);
    struct HistBuild {
        std::vector<std::int32_t> off{0};
        std::vector<double> lo, hi;
        std::vector<std::uint64_t> cnt;
    };
    std::vector<HistBuild> hvals(hist_cols.size());

    const AggSchema& sch = detail::ensure_schema(plan);
    for (const auto& [k, a] : map) {
        (void)k;
        for (std::size_t i = 0; i < group_cols.size(); ++i)
            gk[i].push_back(a.keys[i]);
        std::size_t vc = 0, ti = 0, hi = 0;
        if (count_col) {
            vcells[vc++].push_back(
                FieldNum::of(static_cast<std::int64_t>(a.count)));
        } else {
            for (std::size_t i = 0; i < plan.agg.size(); ++i) {
                const AggOp op = plan.agg[i].op;
                if (op == AggOp::ArgMax) {
                    tvals[ti++].push_back(a.argmax[sch.spec_argmax[i]].repr);
                } else if (op == AggOp::SetUnion) {
                    tvals[ti++].push_back(finalize_set(a, plan, i));
                } else if (op == AggOp::Hist) {
                    auto bins = finalize_hist(a, plan, i);
                    auto& h = hvals[hi++];
                    for (const auto& b : bins) {
                        h.lo.push_back(b.lower);
                        h.hi.push_back(b.upper);
                        h.cnt.push_back(b.count);
                    }
                    h.off.push_back(static_cast<std::int32_t>(h.lo.size()));
                } else {
                    vcells[vc++].push_back(finalize_value_typed(a, plan, i));
                }
            }
        }
        for (const auto& dc : dyn_cols) {
            double v = 0.0;
            if (dc.spec.op == AggOp::Pct) {
                auto it = a.dyn_sketches.find(dc.key);
                if (it != a.dyn_sketches.end())
                    v = it->second.quantile(dc.spec.q);
            } else if (auto it = a.dyn.find(dc.key); it != a.dyn.end()) {
                v = reduce_dyn(it->second, dc.spec.op, dc.spec.q);
            }
            vcells[vc++].push_back(FieldNum::of(v));
        }
        if (occ_cell_col)
            vcells[vc++].push_back(
                FieldNum::of(static_cast<std::int64_t>(sch.occ_cell_us)));
    }

    dataframe::DataFrame batch;
    const std::int64_t nrows = static_cast<std::int64_t>(ng);
    for (std::size_t i = 0; i < group_cols.size(); ++i) {
        batch.names.push_back(group_cols[i]);
        batch.columns.push_back(dataframe::Series::strings(gk[i]));
    }
    auto build_value_column = [&](const std::vector<FieldNum>& cells) {
        FieldStatDomain dom =
            cells.empty() ? FieldStatDomain::F64 : cells[0].domain;
        for (const auto& c : cells)
            if (c.domain != dom) {
                dom = FieldStatDomain::F64;
                break;
            }
        if (dom == FieldStatDomain::I64) {
            std::vector<std::int64_t> v;
            v.reserve(cells.size());
            for (const auto& c : cells) v.push_back(c.i);
            return dataframe::Series::flat(dataframe::TypeId::Int64, v.data(),
                                           nrows);
        }
        if (dom == FieldStatDomain::U64) {
            std::vector<std::uint64_t> v;
            v.reserve(cells.size());
            for (const auto& c : cells) v.push_back(c.u);
            return dataframe::Series::flat(dataframe::TypeId::Uint64, v.data(),
                                           nrows);
        }
        std::vector<double> v;
        v.reserve(cells.size());
        for (const auto& c : cells) v.push_back(c.as_double());
        return dataframe::Series::flat_f64(v.data(), nrows);
    };
    for (std::size_t i = 0; i < value_cols.size(); ++i) {
        batch.names.push_back(value_cols[i]);
        batch.columns.push_back(build_value_column(vcells[i]));
    }
    for (std::size_t i = 0; i < text_cols.size(); ++i) {
        batch.names.push_back(text_cols[i]);
        batch.columns.push_back(dataframe::Series::strings(tvals[i]));
    }
    for (std::size_t i = 0; i < hist_cols.size(); ++i) {
        auto& h = hvals[i];
        const auto n = static_cast<std::int64_t>(h.lo.size());
        std::vector<dataframe::Series> fields;
        fields.push_back(dataframe::Series::flat(dataframe::TypeId::Float64,
                                                 h.lo.data(), n));
        fields.push_back(dataframe::Series::flat(dataframe::TypeId::Float64,
                                                 h.hi.data(), n));
        fields.push_back(dataframe::Series::flat(dataframe::TypeId::Uint64,
                                                 h.cnt.data(), n));
        dataframe::Series st = dataframe::Series::structs({"lo", "hi", "count"},
                                                          std::move(fields));
        batch.names.push_back(hist_cols[i]);
        batch.columns.push_back(dataframe::Series::list(h.off, std::move(st)));
    }
    return batch;
}

// A minimal returning fold over the fused scan (no spill), reproducing the old
// AggFold's per-event/phase/rank handling.
inline bool plan_wants_names(const ViewPlan& plan) {
    return std::any_of(plan.group_by.begin(), plan.group_by.end(),
                       [](const GroupKey& g) {
                           return g.kind == GroupKey::Kind::FilePath ||
                                  g.kind == GroupKey::Kind::FileName ||
                                  g.kind == GroupKey::Kind::HostName;
                       });
}

class OracleAggFold : public detail::Fold {
   public:
    OracleAggFold(const ViewPlan& plan, const StringIntern& intern)
        : plan_(&plan),
          intern_(&intern),
          phase_target_(detail::agg_phase_target(plan)),
          want_ranks_(std::any_of(plan.group_by.begin(), plan.group_by.end(),
                                  [](const GroupKey& g) {
                                      return g.kind == GroupKey::Kind::Rank;
                                  })),
          want_names_(plan_wants_names(plan)) {}

    bool accepts(const detail::ScanShape&) const override { return true; }
    bool needs_args() const override { return true; }
    std::unique_ptr<Fold> slice() const override {
        return std::make_unique<OracleAggFold>(*plan_, *intern_);
    }
    void step(const detail::FoldBatch& batch) override {
        for (const auto& ev : batch.events) {
            if (ev.phase == RecordPhase::METADATA) {
                if (want_ranks_) detail::harvest_pr_rank(ev, *intern_, ranks_);
                if (want_names_) harvest_name(ev);
                if (phase_target_ != RecordPhase::METADATA) continue;
            }
            if (phase_target_ != RecordPhase::UNKNOWN &&
                ev.phase != phase_target_)
                continue;
            detail::PodSource src(ev, *intern_);
            fold_event_over(map_, src, *plan_, keybuf_);
        }
    }
    void seal_unit(const detail::ScanUnit&) override {}
    void drop_unit(const detail::ScanUnit&) override {}
    void merge(Fold& other) override {
        auto& o = static_cast<OracleAggFold&>(other);
        merge_maps(map_, o.map_, *plan_);
        o.map_.clear();
        for (auto& [p, r] : o.ranks_) ranks_.emplace(p, std::move(r));
        o.ranks_.clear();
        for (auto& [h, p] : o.maps_.fpath) maps_.fpath.emplace(h, std::move(p));
        for (auto& [h, n] : o.maps_.host) maps_.host.emplace(h, std::move(n));
        o.maps_.fpath.clear();
        o.maps_.host.clear();
    }
    dftracer::utils::coro::CoroTask<bool> finalize(
        const detail::CoverageSet&) override {
        co_return true;
    }
    GroupMap& map() { return map_; }
    std::unordered_map<std::uint64_t, std::string>& ranks() { return ranks_; }
    HarvestMaps& maps() {
        maps_.rank = ranks_;
        return maps_;
    }

   private:
    // FH/HH records ({"name":"FH","args":{"name":<path>,"value":<hash>}}) give
    // the hash -> path/host maps the resolved group keys need, harvested from
    // the trace itself so the oracle needs no built index name table.
    void harvest_name(const detail::FoldEvent& ev) {
        if (ev.name_id == StringIntern::NO_ID) return;
        const std::string_view rec = intern_->resolve(ev.name_id);
        std::map<std::string, std::string>* dst = nullptr;
        if (rec == "FH")
            dst = &maps_.fpath;
        else if (rec == "HH")
            dst = &maps_.host;
        else
            return;
        std::string_view name, value;
        for (const auto& [kid, val] : ev.args) {
            const auto* sid = std::get_if<std::uint32_t>(&val);
            if (!sid) continue;
            const std::string_view key = intern_->resolve(kid);
            if (key == "name")
                name = intern_->resolve(*sid);
            else if (key == "value")
                value = intern_->resolve(*sid);
        }
        if (!value.empty()) (*dst)[std::string(value)] = std::string(name);
    }

    const ViewPlan* plan_;
    const StringIntern* intern_;
    RecordPhase phase_target_;
    bool want_ranks_ = false;
    bool want_names_ = false;
    GroupMap map_;
    std::string keybuf_;
    std::unordered_map<std::uint64_t, std::string> ranks_;
    HarvestMaps maps_;
};

// The independent GroupMap oracle: fold the scan into a GroupMap, resolve keys
// from the trace's own FH/HH/PR metadata, materialize via to_batch, then apply
// the plan's post-ops. A pure scan with self-contained name resolution, so it
// is a genuine reference for the engine collect.
inline dataframe::DataFrame groupmap_oracle(const detail::scan::ScanPlan& v) {
    ViewPlan plan = detail::resolve_bucket_origin(*v);
    // Scan through throwaway index paths so the oracle never builds or mutates
    // the engine's index (which would make the engine see non-fresh files and
    // skip its bootstrap). The oracle resolves names from harvested metadata,
    // so it needs no name tables of its own. One directory per file set,
    // reused: a fresh one per call rebuilds an index on every scan, which
    // dominates the run under Valgrind.
    static std::map<std::string, std::string> bases;
    std::string key;
    for (const auto& f : plan.files) key += f.file_path + "\n";
    auto it = bases.find(key);
    if (it == bases.end()) {
        std::string base =
            dftu_utils_test::make_unique_test_path("gmoracle").string();
        std::filesystem::create_directories(base);
        it = bases.emplace(std::move(key), std::move(base)).first;
    }
    for (std::size_t i = 0; i < plan.files.size(); ++i)
        plan.files[i].index_path = it->second + "/idx_" + std::to_string(i);
    plan.resolver.reset();
    detail::ensure_schema(plan);
    ViewDefinition vdef = detail::make_vdef(plan, /*for_aggregation=*/true);
    // Keep the FH/HH/PR metadata records so the fold can build its own name and
    // rank maps for resolved group keys.
    if (plan_wants_names(plan) ||
        std::any_of(
            plan.group_by.begin(), plan.group_by.end(),
            [](const GroupKey& g) { return g.kind == GroupKey::Kind::Rank; })) {
        vdef.include_metadata = true;
        vdef.emit_all_metadata = true;
        vdef.filter_metadata = false;
    }
    static dftracer::utils::Runtime rt;
    dataframe::DataFrame result;
    rt.run_blocking("groupmap-oracle",
                    [&](dftracer::utils::CoroScope&)
                        -> dftracer::utils::coro::CoroTask<void> {
                        StringIntern intern;
                        OracleAggFold agg(plan, intern);
                        std::array<detail::Fold*, 1> folds{&agg};
                        co_await detail::fuse(plan, vdef, folds, intern);
                        resolve_group_keys(agg.map(), plan, agg.maps());
                        result = to_batch(agg.map(), plan);
                    });
    return detail::apply_agg_post_ops(std::move(result), plan);
}

}  // namespace gmoracle

#endif  // DFTRACER_TESTS_GROUPMAP_ORACLE_H
