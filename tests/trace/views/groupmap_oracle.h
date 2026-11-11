// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_GROUPMAP_ORACLE_H
#define DFTRACER_UTILS_TESTS_GROUPMAP_ORACLE_H

// An independent, test-local reimplementation of the pre-convergence GroupMap
// aggregation fold (the AggAccum map + to_batch materializer that the dataframe
// engine replaced). It shares only the query-derived schema and the
// key/column-name/resolver helpers with production; the accumulation, merge,
// finalize and materialize logic is its own, so it is a genuine oracle for the
// engine aggregation path rather than a wrapper around it.

#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/reserved_args.h>
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
#include <task_result.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace gmoracle {

struct ArgMaxState {
    double by = 0;
    dftracer::utils::dstd::String repr;
    bool has = false;
};

struct AggAccum {
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> keys;
    std::uint64_t count = 0;
    dftracer::utils::dstd::Vector<dftracer::utils::dataframe::FieldStat> fields;
    dftracer::utils::dstd::Vector<ArgMaxState> argmax;
    dftracer::utils::dstd::Vector<dftracer::utils::dataframe::DDSketch>
        sketches;
    dftracer::utils::dstd::Vector<
        dftracer::utils::dstd::Set<dftracer::utils::dstd::String>>
        sets;
    dftracer::utils::dstd::Map<dftracer::utils::dstd::String,
                               dftracer::utils::dataframe::FieldStat>
        dyn;
    dftracer::utils::dstd::Map<dftracer::utils::dstd::String,
                               dftracer::utils::dataframe::DDSketch>
        dyn_sketches;
    dftracer::utils::dstd::HashMap<std::uint64_t, std::int64_t> occ_deltas;
    std::uint64_t occ_cell_us = 0;
    std::uint64_t occ_total = 0;
    std::uint64_t occ_ts = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t occ_te = 0;
};

using GroupMap =
    dftracer::utils::dstd::HashMap<dftracer::utils::dstd::String, AggAccum>;

// The key text of a missing or JSON-null Arg/Field value: a null output cell.
inline constexpr dftracer::utils::dstd::StringView NULL_KEY{"\0", 1};

template <class Src, class Out>
void append_group_dim(Out& out, const Src& src,
                      const dftracer::utils::trace::views::GroupKey& gk) {
    dftracer::utils::dstd::String d;
    switch (gk.kind) {
        case dftracer::utils::trace::views::GroupKey::Kind::Name:
            src.append_value(d, "name");
            break;
        case dftracer::utils::trace::views::GroupKey::Kind::Cat: {
            const std::size_t start = d.size();
            src.append_value(d, "cat");
            ::dftracer::utils::common::detail::lower_ascii(d, start);
            break;
        }
        case dftracer::utils::trace::views::GroupKey::Kind::Pid:
            src.append_value(d, "pid");
            break;
        case dftracer::utils::trace::views::GroupKey::Kind::Tid:
            src.append_value(d, "tid");
            break;
        case dftracer::utils::trace::views::GroupKey::Kind::Fhash:
            if (!src.append_arg(d, "fhash")) d += NULL_KEY;
            break;
        case dftracer::utils::trace::views::GroupKey::Kind::FilePath:
        case dftracer::utils::trace::views::GroupKey::Kind::FileName:
            src.append_arg(d, "fhash");
            break;
        case dftracer::utils::trace::views::GroupKey::Kind::Hhash:
            if (!src.append_arg(d, "hhash")) d += NULL_KEY;
            break;
        case dftracer::utils::trace::views::GroupKey::Kind::HostName:
            src.append_arg(d, "hhash");
            break;
        case dftracer::utils::trace::views::GroupKey::Kind::IoCat: {
            char buf[8];
            char* p = dftracer::utils::dstd::to_chars_i64(
                buf, buf + sizeof(buf),
                static_cast<int>(dftracer::utils::trace::internal::io_category(
                    src.value("name"))));
            d.append(buf, static_cast<std::size_t>(p - buf));
            break;
        }
        case dftracer::utils::trace::views::GroupKey::Kind::AccPat:
            d.push_back('0');
            break;
        case dftracer::utils::trace::views::GroupKey::Kind::Rank:
            src.append_value(d, "pid");
            break;
        case dftracer::utils::trace::views::GroupKey::Kind::Arg:
            if (!src.append_arg(d, gk.arg)) d += NULL_KEY;
            break;
        case dftracer::utils::trace::views::GroupKey::Kind::Field:
            if (!src.append_value(d, gk.arg)) d += NULL_KEY;
            break;
        case dftracer::utils::trace::views::GroupKey::Kind::Expr:
            // The oracle predates computed keys; no parity case uses one.
            break;
    }
    out.append(d);
}

template <class Src>
dftracer::utils::dstd::String group_dim_str(
    const Src& src, const dftracer::utils::trace::views::GroupKey& gk) {
    dftracer::utils::dstd::String s;
    append_group_dim(s, src, gk);
    return s;
}

template <class Src>
void fold_numeric_args_t(AggAccum& a, const Src& src, bool want_sketch) {
    auto feed = [&](dftracer::utils::dstd::StringView key, double num) {
        a.dyn[dftracer::utils::dstd::String(key)].add(num);
        if (want_sketch)
            a.dyn_sketches[dftracer::utils::dstd::String(key)].add(num);
    };
    if (auto sz = dftracer::utils::trace::views::detail::derived_size_t(src))
        feed("size", *sz);
    src.for_each_numeric_arg([&](dftracer::utils::dstd::StringView key,
                                 double num) {
        if (dftracer::utils::trace::index::schemas::dft::agg::is_reserved_arg(
                key) ||
            dftracer::utils::trace::index::schemas::dft::agg::is_preagg_suffix(
                key))
            return;
        feed(key, num);
    });
}

template <class Src>
void fold_event_over(
    GroupMap& map, const Src& src,
    const dftracer::utils::trace::views::detail::ViewPlan& plan,
    dftracer::utils::dstd::String& keybuf) {
    const dftracer::utils::trace::views::detail::AggSchema& sch = *plan.schema;
    keybuf.clear();
    std::int64_t bucket = 0;
    const bool has_bucket = plan.time_bucket_us > 0;
    if (has_bucket) {
        auto ts = dftracer::utils::trace::views::detail::agg_field_t(src, "ts");
        if (!ts) return;
        const auto interval = static_cast<double>(plan.time_bucket_us);
        const auto w = static_cast<std::int64_t>(plan.time_bucket_us);
        const auto origin = static_cast<std::int64_t>(plan.bucket_origin_us);
        const double rel = *ts * plan.time_scale - static_cast<double>(origin);
        bucket =
            static_cast<std::int64_t>(std::floor(rel / interval)) * w + origin;
        char b[24];
        char* p = dftracer::utils::dstd::to_chars_i64(b, b + sizeof(b), bucket);
        keybuf.append(b, static_cast<std::size_t>(p - b));
        keybuf += dftracer::utils::trace::views::detail::GROUP_SEP;
    }
    for (const auto& gk : plan.group_by) {
        append_group_dim(keybuf, src, gk);
        keybuf += dftracer::utils::trace::views::detail::GROUP_SEP;
    }

    auto it = map.find(keybuf);
    if (it == map.end()) {
        AggAccum a;
        a.keys.reserve(plan.group_by.size() + (has_bucket ? 1 : 0));
        if (has_bucket)
            a.keys.push_back(dftracer::utils::dstd::to_string(bucket));
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
        auto v = dftracer::utils::trace::views::detail::agg_field_typed_t(
            src, sch.fields[fi]);
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
        auto by = dftracer::utils::trace::views::detail::agg_field_t(
            src, plan.agg[i].by);
        if (!by) continue;
        auto& am = a.argmax[slot];
        if (!am.has || *by > am.by) {
            am.by = *by;
            am.repr =
                dftracer::utils::dstd::String(src.value(plan.agg[i].field));
            am.has = true;
        }
    }
    for (std::size_t i = 0; i < plan.agg.size(); ++i) {
        const int slot = sch.spec_set[i];
        if (slot < 0) continue;
        dftracer::utils::dstd::String v(src.value(plan.agg[i].field));
        if (!v.empty()) a.sets[slot].insert(std::move(v));
    }
    if (plan.auto_numeric_metrics) fold_numeric_args_t(a, src, sch.dyn_sketch);
}

inline void merge_accum(
    AggAccum& da, const AggAccum& sa,
    const dftracer::utils::trace::views::detail::ViewPlan& plan) {
    const dftracer::utils::trace::views::detail::AggSchema& sch = *plan.schema;
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

inline void merge_maps(
    GroupMap& dst, const GroupMap& src,
    const dftracer::utils::trace::views::detail::ViewPlan& plan) {
    for (const auto& [k, sa] : src) merge_accum(dst[k], sa, plan);
}

// Name maps harvested from the trace's own FH/HH/PR metadata records, so the
// oracle resolves fhash/hhash/rank group keys without depending on the built
// index's name tables.
struct HarvestMaps {
    dftracer::utils::dstd::Map<dftracer::utils::dstd::String,
                               dftracer::utils::dstd::String>
        fpath;  // fhash -> file path
    dftracer::utils::dstd::Map<dftracer::utils::dstd::String,
                               dftracer::utils::dstd::String>
        host;   // hhash -> host name
    dftracer::utils::dstd::HashMap<std::uint64_t, dftracer::utils::dstd::String>
        rank;   // pid -> rank
    dftracer::utils::dstd::String resolve(
        dftracer::utils::trace::views::GroupKey::Kind kind,
        const dftracer::utils::dstd::String& v) const {
        if (kind == dftracer::utils::trace::views::GroupKey::Kind::FilePath) {
            auto it = fpath.find(v);
            return it != fpath.end() ? it->second : v;
        }
        if (kind == dftracer::utils::trace::views::GroupKey::Kind::FileName) {
            auto it = fpath.find(v);
            const dftracer::utils::dstd::String& p =
                it != fpath.end() ? it->second : v;
            const std::size_t slash = p.find_last_of('/');
            return slash == dftracer::utils::dstd::String::npos
                       ? p
                       : p.substr(slash + 1);
        }
        if (kind == dftracer::utils::trace::views::GroupKey::Kind::HostName) {
            auto it = host.find(v);
            return it != host.end() ? it->second : v;
        }
        if (kind == dftracer::utils::trace::views::GroupKey::Kind::Rank) {
            auto it = rank.find(
                dftracer::utils::dstd::from_chars<std::uint64_t>(v).value());
            return it != rank.end() ? it->second : v;
        }
        return v;
    }
};

inline void resolve_group_keys(
    GroupMap& map, const dftracer::utils::trace::views::detail::ViewPlan& plan,
    const HarvestMaps& maps) {
    const bool needs_resolve = std::any_of(
        plan.group_by.begin(), plan.group_by.end(), [](const auto& g) {
            return g.kind == dftracer::utils::trace::views::GroupKey::Kind::
                                 FilePath ||
                   g.kind == dftracer::utils::trace::views::GroupKey::Kind::
                                 FileName ||
                   g.kind == dftracer::utils::trace::views::GroupKey::Kind::
                                 HostName ||
                   g.kind ==
                       dftracer::utils::trace::views::GroupKey::Kind::Rank;
        });
    const bool any_transform = std::any_of(
        plan.group_by.begin(), plan.group_by.end(), [](const auto& g) {
            return g.transform !=
                   dftracer::utils::trace::views::GroupKey::Transform::None;
        });
    if (!needs_resolve && !any_transform) return;
    const std::size_t off = plan.time_bucket_us > 0 ? 1 : 0;
    GroupMap out;
    dftracer::utils::dstd::String newkey;
    for (auto& entry : map) {
        auto& accum = entry.second;
        for (std::size_t j = 0; j < plan.group_by.size(); ++j) {
            const std::size_t idx = off + j;
            if (idx >= accum.keys.size()) continue;
            dftracer::utils::dstd::String v =
                maps.resolve(plan.group_by[j].kind, std::move(accum.keys[idx]));
            accum.keys[idx] = dftracer::utils::dstd::String(
                dftracer::utils::trace::views::detail::apply_group_transform(
                    plan.group_by[j], dftracer::utils::dstd::String(v)));
        }
        newkey.clear();
        for (const auto& part : accum.keys) {
            newkey += part;
            newkey += dftracer::utils::trace::views::detail::GROUP_SEP;
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
    dftracer::utils::dstd::Vector<
        dftracer::utils::dstd::Pair<std::uint64_t, std::int64_t>>
        pts(a.occ_deltas.begin(), a.occ_deltas.end());
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

inline double finalize_value(
    const AggAccum& a,
    const dftracer::utils::trace::views::detail::ViewPlan& plan,
    std::size_t i) {
    const dftracer::utils::trace::views::detail::AggSchema& sch = *plan.schema;
    const auto& spec = plan.agg[i];
    const int fi = sch.spec_field[i];
    const dftracer::utils::dataframe::FieldStat* fs =
        fi >= 0 ? &a.fields[fi] : nullptr;
    const double N = static_cast<double>(a.count);
    switch (spec.op) {
        case dftracer::utils::trace::views::AggOp::Count:
            return spec.field.empty() ? N
                                      : (fs ? static_cast<double>(fs->n) : 0.0);
        case dftracer::utils::trace::views::AggOp::Sum:
            return fs ? fs->sum : 0.0;
        case dftracer::utils::trace::views::AggOp::Min:
            return fs ? fs->min : 0.0;
        case dftracer::utils::trace::views::AggOp::Max:
            return fs ? fs->max : 0.0;
        case dftracer::utils::trace::views::AggOp::SumSq:
            return fs ? fs->sumsq() : 0.0;
        case dftracer::utils::trace::views::AggOp::Mean:
            return fs ? fs->mean() : 0.0;
        case dftracer::utils::trace::views::AggOp::Var:
            return fs ? fs->variance(true) : 0.0;
        case dftracer::utils::trace::views::AggOp::Std:
            return fs ? fs->stddev(true) : 0.0;
        case dftracer::utils::trace::views::AggOp::Pct: {
            const int sk = fi >= 0 ? sch.field_sketch[fi] : -1;
            return sk >= 0 ? a.sketches[sk].quantile(spec.q) : 0.0;
        }
        case dftracer::utils::trace::views::AggOp::Skew:
            return fs ? fs->skewness() : 0.0;
        case dftracer::utils::trace::views::AggOp::Kurt:
            return fs ? fs->kurtosis() : 0.0;
        case dftracer::utils::trace::views::AggOp::ArgMax:
        case dftracer::utils::trace::views::AggOp::Hist:
        case dftracer::utils::trace::views::AggOp::SetUnion:
            return 0.0;
        case dftracer::utils::trace::views::AggOp::Busy:
            return static_cast<double>(occupancy_summary(a).busy);
        case dftracer::utils::trace::views::AggOp::Concurrency: {
            const OccSummary o = occupancy_summary(a);
            return o.busy ? static_cast<double>(o.total) /
                                static_cast<double>(o.busy)
                          : 0.0;
        }
        case dftracer::utils::trace::views::AggOp::Utilization: {
            const OccSummary o = occupancy_summary(a);
            return (o.busy && o.span) ? static_cast<double>(o.busy) /
                                            static_cast<double>(o.span)
                                      : 0.0;
        }
        case dftracer::utils::trace::views::AggOp::Active:
            return static_cast<double>(occupancy_summary(a).active);
    }
    return 0.0;
}

inline dftracer::utils::dataframe::FieldNum finalize_value_typed(
    const AggAccum& a,
    const dftracer::utils::trace::views::detail::ViewPlan& plan,
    std::size_t i) {
    const dftracer::utils::trace::views::detail::AggSchema& sch = *plan.schema;
    const auto& spec = plan.agg[i];
    const int fi = sch.spec_field[i];
    const dftracer::utils::dataframe::FieldStat* fs =
        fi >= 0 ? &a.fields[fi] : nullptr;
    auto exact = [&](std::int64_t bits,
                     double dbl) -> dftracer::utils::dataframe::FieldNum {
        if (!fs) return dftracer::utils::dataframe::FieldNum::of(dbl);
        switch (fs->domain) {
            case dftracer::utils::dataframe::FieldStatDomain::I64:
                return dftracer::utils::dataframe::FieldNum::of(bits);
            case dftracer::utils::dataframe::FieldStatDomain::U64:
                return dftracer::utils::dataframe::FieldNum::of(
                    std::bit_cast<std::uint64_t>(bits));
            default:
                return dftracer::utils::dataframe::FieldNum::of(dbl);
        }
    };
    switch (spec.op) {
        case dftracer::utils::trace::views::AggOp::Count:
            return dftracer::utils::dataframe::FieldNum::of(
                static_cast<std::int64_t>(
                    spec.field.empty() ? a.count : (fs ? fs->n : 0)));
        case dftracer::utils::trace::views::AggOp::Sum:
            return fs ? exact(fs->esum, fs->sum)
                      : dftracer::utils::dataframe::FieldNum::of(0.0);
        case dftracer::utils::trace::views::AggOp::Min:
            return fs ? exact(fs->emin, fs->min)
                      : dftracer::utils::dataframe::FieldNum::of(0.0);
        case dftracer::utils::trace::views::AggOp::Max:
            return fs ? exact(fs->emax, fs->max)
                      : dftracer::utils::dataframe::FieldNum::of(0.0);
        default:
            return dftracer::utils::dataframe::FieldNum::of(
                finalize_value(a, plan, i));
    }
}

inline dftracer::utils::dstd::String finalize_set(
    const AggAccum& a,
    const dftracer::utils::trace::views::detail::ViewPlan& plan,
    std::size_t i) {
    const int si = plan.schema->spec_set[i];
    if (si < 0 || static_cast<std::size_t>(si) >= a.sets.size()) return {};
    dftracer::utils::dstd::String out;
    for (const auto& v : a.sets[si]) {
        if (!out.empty())
            out.push_back(dftracer::utils::trace::views::detail::SET_SEP);
        out += v;
    }
    return out;
}

inline dftracer::utils::dstd::Vector<dftracer::utils::dataframe::HistogramBin>
finalize_hist(const AggAccum& a,
              const dftracer::utils::trace::views::detail::ViewPlan& plan,
              std::size_t i) {
    const dftracer::utils::trace::views::detail::AggSchema& sch = *plan.schema;
    const int fi = sch.spec_field[i];
    const int sk = fi >= 0 ? sch.field_sketch[fi] : -1;
    if (sk < 0) return {};
    return a.sketches[sk].bins();
}

inline double reduce_dyn(const dftracer::utils::dataframe::FieldStat& fs,
                         dftracer::utils::trace::views::AggOp op, double) {
    switch (op) {
        case dftracer::utils::trace::views::AggOp::Count:
            return static_cast<double>(fs.n);
        case dftracer::utils::trace::views::AggOp::Sum:
            return fs.sum;
        case dftracer::utils::trace::views::AggOp::Min:
            return fs.n ? fs.min : 0.0;
        case dftracer::utils::trace::views::AggOp::Max:
            return fs.n ? fs.max : 0.0;
        case dftracer::utils::trace::views::AggOp::SumSq:
            return fs.sumsq();
        case dftracer::utils::trace::views::AggOp::Mean:
            return fs.mean();
        case dftracer::utils::trace::views::AggOp::Var:
            return fs.variance(true);
        case dftracer::utils::trace::views::AggOp::Std:
            return fs.stddev(true);
        case dftracer::utils::trace::views::AggOp::Skew:
            return fs.skewness();
        case dftracer::utils::trace::views::AggOp::Kurt:
            return fs.kurtosis();
        default:
            return 0.0;
    }
}

inline dftracer::utils::dataframe::DataFrame to_batch(
    const GroupMap& map,
    const dftracer::utils::trace::views::detail::ViewPlan& plan) {
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> group_cols,
        value_cols, text_cols, hist_cols;
    if (plan.time_bucket_us > 0) group_cols.push_back("time_bucket");
    for (const auto& gk : plan.group_by)
        group_cols.push_back(dftracer::utils::dstd::String(
            dftracer::utils::trace::views::detail::group_col_name(gk)));
    const bool count_col = plan.agg.empty();
    if (count_col) {
        value_cols.push_back("count");
    } else {
        for (const auto& spec : plan.agg) {
            if (spec.op == dftracer::utils::trace::views::AggOp::ArgMax ||
                spec.op == dftracer::utils::trace::views::AggOp::SetUnion)
                text_cols.push_back(dftracer::utils::dstd::String(
                    dftracer::utils::trace::views::detail::agg_col_name(spec)));
            else if (spec.op == dftracer::utils::trace::views::AggOp::Hist)
                hist_cols.push_back(dftracer::utils::dstd::String(
                    dftracer::utils::trace::views::detail::agg_col_name(spec)));
            else
                value_cols.push_back(dftracer::utils::dstd::String(
                    dftracer::utils::trace::views::detail::agg_col_name(spec)));
        }
    }
    struct DynCol {
        dftracer::utils::dstd::String key;
        dftracer::utils::trace::views::AggSpec spec;
    };
    dftracer::utils::dstd::Vector<DynCol> dyn_cols;
    if (plan.auto_numeric_metrics) {
        dftracer::utils::dstd::Set<dftracer::utils::dstd::String> names;
        for (const auto& entry : map) {
            for (const auto& [name, m] : entry.second.dyn) names.insert(name);
        }
        if (plan.numeric_arg_aggs.empty()) {
            for (const auto& n : names) {
                dyn_cols.push_back(
                    {n, dftracer::utils::trace::views::AggSpec(
                            dftracer::utils::trace::views::AggOp::Mean)});
                value_cols.push_back(n);
            }
        } else {
            for (const auto& n : names)
                for (const auto& spec : plan.numeric_arg_aggs) {
                    dyn_cols.push_back({n, spec});
                    value_cols.push_back(dftracer::utils::dstd::String(
                        dftracer::utils::trace::views::detail::dyn_col_name(
                            spec, dftracer::utils::dstd::String(n))));
                }
        }
    }
    bool occ_cell_col = false;
    for (const auto& spec : plan.agg)
        if (spec.op == dftracer::utils::trace::views::AggOp::Busy ||
            spec.op == dftracer::utils::trace::views::AggOp::Concurrency ||
            spec.op == dftracer::utils::trace::views::AggOp::Utilization) {
            occ_cell_col = true;
            break;
        }
    if (occ_cell_col) value_cols.push_back("busy_cell_us");

    const std::size_t ng = map.size();
    dftracer::utils::dstd::Vector<
        dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>>
        gk(group_cols.size());
    for (auto& v : gk) v.reserve(ng);
    dftracer::utils::dstd::Vector<
        dftracer::utils::dstd::Vector<dftracer::utils::dataframe::FieldNum>>
        vcells(value_cols.size());
    for (auto& v : vcells) v.reserve(ng);
    dftracer::utils::dstd::Vector<
        dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>>
        tvals(text_cols.size());
    for (auto& v : tvals) v.reserve(ng);
    struct HistBuild {
        dftracer::utils::dstd::Vector<std::int32_t> off{0};
        dftracer::utils::dstd::Vector<double> lo, hi;
        dftracer::utils::dstd::Vector<std::uint64_t> cnt;
    };
    dftracer::utils::dstd::Vector<HistBuild> hvals(hist_cols.size());

    const dftracer::utils::trace::views::detail::AggSchema& sch =
        dftracer::utils::trace::views::detail::ensure_schema(plan);
    for (const auto& entry : map) {
        const auto& a = entry.second;
        for (std::size_t i = 0; i < group_cols.size(); ++i)
            gk[i].push_back(a.keys[i]);
        std::size_t vc = 0, ti = 0, hi = 0;
        if (count_col) {
            vcells[vc++].push_back(dftracer::utils::dataframe::FieldNum::of(
                static_cast<std::int64_t>(a.count)));
        } else {
            for (std::size_t i = 0; i < plan.agg.size(); ++i) {
                const dftracer::utils::trace::views::AggOp op = plan.agg[i].op;
                if (op == dftracer::utils::trace::views::AggOp::ArgMax) {
                    tvals[ti++].push_back(a.argmax[sch.spec_argmax[i]].repr);
                } else if (op ==
                           dftracer::utils::trace::views::AggOp::SetUnion) {
                    tvals[ti++].push_back(finalize_set(a, plan, i));
                } else if (op == dftracer::utils::trace::views::AggOp::Hist) {
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
            if (dc.spec.op == dftracer::utils::trace::views::AggOp::Pct) {
                auto it = a.dyn_sketches.find(dc.key);
                if (it != a.dyn_sketches.end())
                    v = it->second.quantile(dc.spec.q);
            } else if (auto it = a.dyn.find(dc.key); it != a.dyn.end()) {
                v = reduce_dyn(it->second, dc.spec.op, dc.spec.q);
            }
            vcells[vc++].push_back(dftracer::utils::dataframe::FieldNum::of(v));
        }
        if (occ_cell_col)
            vcells[vc++].push_back(dftracer::utils::dataframe::FieldNum::of(
                static_cast<std::int64_t>(sch.occ_cell_us)));
    }

    dftracer::utils::dataframe::DataFrame batch;
    const std::int64_t nrows = static_cast<std::int64_t>(ng);
    for (std::size_t i = 0; i < group_cols.size(); ++i) {
        batch.names.push_back(group_cols[i]);
        dftracer::utils::dstd::Vector<dftracer::utils::dstd::StringView> views(
            gk[i].begin(), gk[i].end());
        dftracer::utils::dstd::Vector<std::uint8_t> vbits(
            (views.size() + 7) / 8, 0);
        for (std::size_t r = 0; r < views.size(); ++r)
            if (views[r] == NULL_KEY)
                views[r] = {};
            else
                vbits[r >> 3] |= static_cast<std::uint8_t>(1u << (r & 7));
        batch.columns.push_back(dftracer::utils::dataframe::Series::strings(
            dftracer::utils::dstd::Span<
                const dftracer::utils::dstd::StringView>(views),
            vbits.data()));
    }
    auto build_value_column =
        [&](const dftracer::utils::dstd::Vector<
            dftracer::utils::dataframe::FieldNum>& cells) {
            dftracer::utils::dataframe::FieldStatDomain dom =
                cells.empty() ? dftracer::utils::dataframe::FieldStatDomain::F64
                              : cells[0].domain;
            for (const auto& c : cells)
                if (c.domain != dom) {
                    dom = dftracer::utils::dataframe::FieldStatDomain::F64;
                    break;
                }
            if (dom == dftracer::utils::dataframe::FieldStatDomain::I64) {
                dftracer::utils::dstd::Vector<std::int64_t> v;
                v.reserve(cells.size());
                for (const auto& c : cells) v.push_back(c.i);
                return dftracer::utils::dataframe::Series::flat(
                    dftracer::utils::dataframe::TypeId::Int64, v.data(), nrows);
            }
            if (dom == dftracer::utils::dataframe::FieldStatDomain::U64) {
                dftracer::utils::dstd::Vector<std::uint64_t> v;
                v.reserve(cells.size());
                for (const auto& c : cells) v.push_back(c.u);
                return dftracer::utils::dataframe::Series::flat(
                    dftracer::utils::dataframe::TypeId::Uint64, v.data(),
                    nrows);
            }
            dftracer::utils::dstd::Vector<double> v;
            v.reserve(cells.size());
            for (const auto& c : cells) v.push_back(c.as_double());
            return dftracer::utils::dataframe::Series::flat_f64(v.data(),
                                                                nrows);
        };
    for (std::size_t i = 0; i < value_cols.size(); ++i) {
        batch.names.push_back(value_cols[i]);
        batch.columns.push_back(build_value_column(vcells[i]));
    }
    for (std::size_t i = 0; i < text_cols.size(); ++i) {
        batch.names.push_back(text_cols[i]);
        batch.columns.push_back(
            dftracer::utils::dataframe::Series::strings(tvals[i]));
    }
    for (std::size_t i = 0; i < hist_cols.size(); ++i) {
        auto& h = hvals[i];
        const auto n = static_cast<std::int64_t>(h.lo.size());
        dftracer::utils::dstd::Vector<dftracer::utils::dataframe::Series>
            fields;
        fields.push_back(dftracer::utils::dataframe::Series::flat(
            dftracer::utils::dataframe::TypeId::Float64, h.lo.data(), n));
        fields.push_back(dftracer::utils::dataframe::Series::flat(
            dftracer::utils::dataframe::TypeId::Float64, h.hi.data(), n));
        fields.push_back(dftracer::utils::dataframe::Series::flat(
            dftracer::utils::dataframe::TypeId::Uint64, h.cnt.data(), n));
        dftracer::utils::dataframe::Series st =
            dftracer::utils::dataframe::Series::structs({"lo", "hi", "count"},
                                                        std::move(fields));
        batch.names.push_back(hist_cols[i]);
        batch.columns.push_back(
            dftracer::utils::dataframe::Series::list(h.off, std::move(st)));
    }
    return batch;
}

// A minimal returning fold over the fused scan (no spill), reproducing the old
// AggFold's per-event/phase/rank handling.
inline bool plan_wants_names(
    const dftracer::utils::trace::views::detail::ViewPlan& plan) {
    return std::any_of(
        plan.group_by.begin(), plan.group_by.end(),
        [](const dftracer::utils::trace::views::GroupKey& g) {
            return g.kind == dftracer::utils::trace::views::GroupKey::Kind::
                                 FilePath ||
                   g.kind == dftracer::utils::trace::views::GroupKey::Kind::
                                 FileName ||
                   g.kind ==
                       dftracer::utils::trace::views::GroupKey::Kind::HostName;
        });
}

// pid -> rank from a PR metadata event ({"name":"PR","pid":P,
// "args":{"name":"rank","value":"N"}}); others are ignored.
inline void harvest_pr_rank(
    const dftracer::utils::trace::views::detail::FoldEvent& ev,
    const dftracer::utils::dstd::StringIntern& intern,
    dftracer::utils::dstd::HashMap<std::uint64_t,
                                   dftracer::utils::dstd::String>& ranks) {
    if (ev.name_id == dftracer::utils::dstd::StringIntern::NO_ID ||
        intern.resolve(ev.name_id) != "PR")
        return;
    dftracer::utils::dstd::StringView rank;
    bool is_rank = false;
    for (const auto& [kid, v] : ev.args) {
        const auto* sid = std::get_if<std::uint32_t>(&v);
        if (!sid) continue;
        const dftracer::utils::dstd::StringView key = intern.resolve(kid);
        if (key == "name")
            is_rank = intern.resolve(*sid) == "rank";
        else if (key == "value")
            rank = intern.resolve(*sid);
    }
    if (is_rank && !rank.empty())
        ranks[ev.pid] = dftracer::utils::dstd::String(rank);
}

class OracleAggFold : public dftracer::utils::trace::views::detail::Fold {
   public:
    OracleAggFold(const dftracer::utils::trace::views::detail::ViewPlan& plan,
                  const dftracer::utils::dstd::StringIntern& intern)
        : plan_(&plan),
          intern_(&intern),
          phase_target_(
              dftracer::utils::trace::views::detail::agg_phase_target(plan)),
          want_ranks_(std::any_of(
              plan.group_by.begin(), plan.group_by.end(),
              [](const dftracer::utils::trace::views::GroupKey& g) {
                  return g.kind ==
                         dftracer::utils::trace::views::GroupKey::Kind::Rank;
              })),
          want_names_(plan_wants_names(plan)) {}

    bool accepts(const dftracer::utils::trace::views::detail::ScanShape&)
        const override {
        return true;
    }
    bool needs_args() const override { return true; }
    dftracer::utils::dstd::UniquePtr<Fold> slice() const override {
        return dftracer::utils::dstd::make_unique<OracleAggFold>(*plan_,
                                                                 *intern_);
    }
    dftracer::utils::dstd::Result<void> step(
        const dftracer::utils::trace::views::detail::FoldBatch& batch)
        override {
        for (const auto& ev : batch.events) {
            if (ev.phase == dftracer::utils::trace::RecordPhase::Metadata) {
                if (want_ranks_) harvest_pr_rank(ev, *intern_, ranks_);
                if (want_names_) harvest_name(ev);
                if (phase_target_ !=
                    dftracer::utils::trace::RecordPhase::Metadata)
                    continue;
            }
            if (phase_target_ != dftracer::utils::trace::RecordPhase::Unknown &&
                ev.phase != phase_target_)
                continue;
            dftracer::utils::trace::views::detail::PodSource src(ev, *intern_);
            fold_event_over(map_, src, *plan_, keybuf_);
        }
        return {};
    }
    void seal_unit(
        const dftracer::utils::trace::views::detail::ScanUnit&) override {}
    void drop_unit(
        const dftracer::utils::trace::views::detail::ScanUnit&) override {}
    dftracer::utils::dstd::Result<void> merge(Fold& other) override {
        auto& o = static_cast<OracleAggFold&>(other);
        merge_maps(map_, o.map_, *plan_);
        o.map_.clear();
        for (auto& [p, r] : o.ranks_) ranks_.emplace(p, std::move(r));
        o.ranks_.clear();
        for (auto& [h, p] : o.maps_.fpath) maps_.fpath.emplace(h, std::move(p));
        for (auto& [h, n] : o.maps_.host) maps_.host.emplace(h, std::move(n));
        o.maps_.fpath.clear();
        o.maps_.host.clear();
        return {};
    }
    dftracer::utils::coro::CoroTask<bool> finalize(
        const dftracer::utils::trace::views::detail::CoverageSet&) override {
        co_return true;
    }
    GroupMap& map() { return map_; }
    dftracer::utils::dstd::HashMap<std::uint64_t,
                                   dftracer::utils::dstd::String>&
    ranks() {
        return ranks_;
    }
    HarvestMaps& maps() {
        maps_.rank = ranks_;
        return maps_;
    }

   private:
    // FH/HH records ({"name":"FH","args":{"name":<path>,"value":<hash>}}) give
    // the hash -> path/host maps the resolved group keys need, harvested from
    // the trace itself so the oracle needs no built index name table.
    void harvest_name(
        const dftracer::utils::trace::views::detail::FoldEvent& ev) {
        if (ev.name_id == dftracer::utils::dstd::StringIntern::NO_ID) return;
        const dftracer::utils::dstd::StringView rec =
            intern_->resolve(ev.name_id);
        dftracer::utils::dstd::Map<dftracer::utils::dstd::String,
                                   dftracer::utils::dstd::String>* dst =
            nullptr;
        if (rec == "FH")
            dst = &maps_.fpath;
        else if (rec == "HH")
            dst = &maps_.host;
        else
            return;
        dftracer::utils::dstd::StringView name, value;
        for (const auto& [kid, val] : ev.args) {
            const auto* sid = std::get_if<std::uint32_t>(&val);
            if (!sid) continue;
            const dftracer::utils::dstd::StringView key = intern_->resolve(kid);
            if (key == "name")
                name = intern_->resolve(*sid);
            else if (key == "value")
                value = intern_->resolve(*sid);
        }
        if (!value.empty())
            (*dst)[dftracer::utils::dstd::String(value)] =
                dftracer::utils::dstd::String(name);
    }

    const dftracer::utils::trace::views::detail::ViewPlan* plan_;
    const dftracer::utils::dstd::StringIntern* intern_;
    dftracer::utils::trace::RecordPhase phase_target_;
    bool want_ranks_ = false;
    bool want_names_ = false;
    GroupMap map_;
    dftracer::utils::dstd::String keybuf_;
    dftracer::utils::dstd::HashMap<std::uint64_t, dftracer::utils::dstd::String>
        ranks_;
    HarvestMaps maps_;
};

// The independent GroupMap oracle: fold the scan into a GroupMap, resolve keys
// from the trace's own FH/HH/PR metadata, materialize via to_batch, then apply
// the plan's post-ops. A pure scan with self-contained name resolution, so it
// is a genuine reference for the engine collect.
inline dftracer::utils::dataframe::DataFrame groupmap_oracle(
    const dftracer::utils::trace::views::detail::scan::ScanPlan& v) {
    dftracer::utils::trace::views::detail::ViewPlan plan =
        dftracer::utils::trace::views::detail::resolve_bucket_origin(*v);
    // Scan through throwaway index paths so the oracle never builds or mutates
    // the engine's index (which would make the engine see non-fresh files and
    // skip its bootstrap). The oracle resolves names from harvested metadata,
    // so it needs no name tables of its own. One directory per file set,
    // reused: a fresh one per call rebuilds an index on every scan, which
    // dominates the run under Valgrind.
    static dftracer::utils::dstd::Map<dftracer::utils::dstd::String,
                                      dftracer::utils::dstd::String>
        bases;
    dftracer::utils::dstd::String key;
    for (const auto& f : plan.files) key += f.file_path + "\n";
    auto it = bases.find(key);
    if (it == bases.end()) {
        dftracer::utils::dstd::String base = dftracer::utils::dstd::String(
            dftu_utils_test::make_unique_test_path("gmoracle").str());
        REQUIRE(
            dftracer::utils::dstd::fs::create_directories(base).has_value());
        it = bases.emplace(std::move(key), std::move(base)).first;
    }
    for (std::size_t i = 0; i < plan.files.size(); ++i)
        plan.files[i].index_path =
            it->second + "/idx_" + dftracer::utils::dstd::to_string(i);
    plan.resolver.reset();
    dftracer::utils::trace::views::detail::ensure_schema(plan);
    dftracer::utils::trace::views::ViewDefinition vdef =
        dftu_utils_test::ok(dftracer::utils::trace::views::detail::make_vdef(
            plan, /*for_aggregation=*/true));
    // Keep the FH/HH/PR metadata records so the fold can build its own name and
    // rank maps for resolved group keys.
    if (plan_wants_names(plan) ||
        std::any_of(
            plan.group_by.begin(), plan.group_by.end(),
            [](const dftracer::utils::trace::views::GroupKey& g) {
                return g.kind ==
                       dftracer::utils::trace::views::GroupKey::Kind::Rank;
            })) {
        vdef.include_metadata = true;
        vdef.filter_metadata = false;
    }
    static dftracer::utils::Runtime rt;
    dftracer::utils::dataframe::DataFrame result;
    dftu_utils_test::ok(rt.block_on(dftracer::utils::coro::named(
        "groupmap-oracle",
        dftracer::utils::coro::with_scope(
            [&](dftracer::utils::coro::Scope&)
                -> dftracer::utils::coro::CoroTask<void> {
                dftracer::utils::dstd::StringIntern intern;
                OracleAggFold agg(plan, intern);
                std::array<dftracer::utils::trace::views::detail::Fold*, 1>
                    folds{&agg};
                dftu_utils_test::ok(
                    co_await dftracer::utils::trace::views::detail::fuse(
                        plan, vdef, folds, intern));
                resolve_group_keys(agg.map(), plan, agg.maps());
                result = to_batch(agg.map(), plan);
            }))));
    return dftu_utils_test::ok(
        dftracer::utils::trace::views::detail::apply_agg_post_ops(
            std::move(result), plan));
}

}  // namespace gmoracle

#endif  // DFTRACER_UTILS_TESTS_GROUPMAP_ORACLE_H
