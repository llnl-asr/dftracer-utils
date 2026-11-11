// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/hash/hex64.h>
#include <dftracer/utils/dstd/hash/xxh3.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/detail/ast.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/duql/term.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/trace/index/extensions/kinds/payloads.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/plan/view_resolver.h>
#include <dftracer/utils/trace/index/record_schema.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/system_metrics.h>
#include <dftracer/utils/trace/index/store/layout.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_agg_partials.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_scan.h>

#include <algorithm>
#include <bit>
#include <type_traits>
#include <utility>
#include <variant>

namespace dftracer::utils::trace::views::detail {

namespace {

using fileio::get_logical_path;

constexpr char EXTRA_SEP = '\x1f';

// A path-decoded role maps to the tier's field only when the tier holds exactly
// the value the scan reads: a required field, in microseconds for times. Empty
// when it does not.
dstd::String tier_field(const ViewPlan& plan, dstd::StringView f) {
    if (!plan_by_path(plan)) return dstd::String(f);
    const trace::index::RecordSchema& s = plan_record_schema(plan);
    const trace::index::Roles& r = s.roles;
    auto exact = [&](dstd::StringView path, trace::index::FieldType type) {
        if (f.empty() || path != f) return false;
        const trace::index::FieldSpec* spec = s.field_at(path);
        return spec && !spec->optional && spec->type == type;
    };
    if (exact(r.name, trace::index::FieldType::String)) return "name";
    if (exact(r.entity, trace::index::FieldType::Int)) return "pid";
    if (exact(r.lane, trace::index::FieldType::Int)) return "tid";
    if (r.duration_unit == trace::index::TimeUnit::Us &&
        exact(r.duration, trace::index::FieldType::Int))
        return "dur";
    if (r.time_unit == trace::index::TimeUnit::Us &&
        exact(r.time, trace::index::FieldType::Int))
        return "ts";
    return {};
}

// The tier folds a missing or negative role value as 0 where the scan reads
// null, so every chunk's zone must show the role present in every record.
dstd::Result<bool> roles_complete(const ViewPlan& plan) {
    const trace::index::Roles& r = plan_record_schema(plan).roles;
    dstd::Vector<dstd::StringView> paths;
    for (const dstd::String* p :
         {&r.name, &r.entity, &r.lane, &r.duration, &r.time})
        if (!tier_field(plan, *p).empty()) paths.push_back(*p);
    if (paths.empty()) return true;
    dstd::Map<dstd::String, trace::index::Snapshot> snapshots;
    for (const auto& f : plan.files) {
        auto at = snapshots.find(f.index_path);
        if (at == snapshots.end()) {
            auto opened = snapshot_of(plan, f.index_path);
            if (!opened) return opened.error();
            at = snapshots.emplace(f.index_path, std::move(*opened)).first;
        }
        const trace::index::Snapshot& snapshot = at->second;
        const auto file = snapshot.file(get_logical_path(f.file_path));
        if (!file ||
            !trace::index::extension_current(
                snapshot, *file, trace::index::store::IndexExtension::Zonemap))
            return false;
        const auto spans = trace::index::spans(snapshot, *file);
        if (!spans) return spans.error();
        for (dstd::StringView p : paths) {
            dstd::HashMap<
                std::uint64_t,
                dftracer::utils::trace::index::extensions::kinds::Zone>
                zones;
            const auto granules = trace::index::granules(
                snapshot, *file, trace::index::store::IndexExtension::Zonemap,
                p);
            if (!granules) return granules.error();
            for (const auto& [granule, bytes] : *granules)
                if (auto z = dftracer::utils::trace::index::extensions::kinds::
                        decode_zone(bytes))
                    zones.emplace(granule, std::move(*z));
            for (std::uint64_t i = 0; i < spans->size(); ++i) {
                const auto& span = (*spans)[i];
                if (span.first_line_num == 0 ||
                    span.last_line_num < span.first_line_num)
                    continue;
                const std::uint64_t lines =
                    span.last_line_num - span.first_line_num + 1;
                const auto z = zones.find(i);
                if (z == zones.end() || z->second.observed != lines ||
                    z->second.present != lines ||
                    z->second.min.starts_with('-'))
                    return false;
            }
        }
    }
    return true;
}

bool answerable_field(const dstd::String& f) {
    return f == "dur" || f == "size" || f == "offset" || f == "ts" || f == "te";
}

// The partials hold every record, and a path schema's `data` row set can leave
// some out.
bool rows_match(const ViewPlan& plan) {
    return !plan_by_path(plan) || plan.all_records ||
           plan_record_schema(plan).data.empty();
}

// nullopt when a key is one the partials cannot answer.
dstd::Optional<dstd::Vector<GroupKey>> partial_keys(const ViewPlan& plan) {
    using Kind = GroupKey::Kind;
    dstd::Vector<GroupKey> keys = plan.group_by;
    if (plan_by_path(plan)) {
        for (auto& gk : keys) {
            if (gk.kind != Kind::Field) return dstd::nullopt;
            const dstd::String t = tier_field(plan, gk.arg);
            if (t == "name")
                gk.kind = Kind::Name;
            else if (t == "pid")
                gk.kind = Kind::Pid;
            else if (t == "tid")
                gk.kind = Kind::Tid;
            else
                return dstd::nullopt;
        }
        return keys;
    }
    for (const auto& gk : keys) {
        switch (gk.kind) {
            case Kind::Name:
            case Kind::Cat:
            case Kind::Pid:
            case Kind::Tid:
            case Kind::Fhash:
            case Kind::Hhash:
            case Kind::IoCat:
            case Kind::AccPat:
            case Kind::FilePath:
            case Kind::FileName:
            case Kind::HostName:
                break;
            case Kind::Arg:
                break;
            case Kind::Rank:
            case Kind::Field:
            case Kind::Expr:
                return dstd::nullopt;
        }
    }
    return keys;
}

bool reads_lookup(const duql::detail::QueryNode& node) {
    return std::visit(
        [](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::detail::AndNode> ||
                          std::is_same_v<T, duql::detail::OrNode>) {
                return reads_lookup(*n.left) || reads_lookup(*n.right);
            } else if constexpr (std::is_same_v<T, duql::detail::NotNode>) {
                return reads_lookup(*n.operand);
            } else if constexpr (std::is_same_v<T, duql::detail::ExprLeaf>) {
                bool found = false;
                duql::for_each_term(*n.term, [&found](const duql::Term& t) {
                    found =
                        found || std::holds_alternative<duql::TLookup>(t.node);
                });
                return found;
            } else {
                return false;
            }
        },
        node.data);
}

bool query_answerable(const ViewPlan& plan) {
    const duql::Query& q = *plan.query;
    // Aggregated records keep no array elements.
    if (!q.any_paths().empty()) return false;
    // The view binds a row set's rows only on the scan path.
    if (reads_lookup(duql::detail::QueryAccess::root(q))) return false;
    for (dstd::StringView f : q.fields()) {
        const dstd::String t = tier_field(plan, f);
        if (t != "name" && t != "pid" && t != "tid" && t != "fhash" &&
            t != "hhash" && t != "cat")
            return false;
    }
    return true;
}

// No default, so -Wswitch flags a newly added op.
bool serves_op(AggOp op) {
    switch (op) {
        case AggOp::Count:
        case AggOp::Sum:
        case AggOp::Min:
        case AggOp::Max:
        case AggOp::Mean:
        case AggOp::Var:
        case AggOp::Std:
        case AggOp::Skew:
        case AggOp::Kurt:
        case AggOp::SumSq:
        case AggOp::Pct:
        case AggOp::Hist:
            return true;
        case AggOp::ArgMax:
        case AggOp::SetUnion:
        case AggOp::Busy:
        case AggOp::Concurrency:
        case AggOp::Utilization:
        case AggOp::Active:
            return false;
    }
    return false;
}

// A phrase for the decline log, empty when the partials answer every aggregate.
dstd::String unanswered_agg(const ViewPlan& plan, const AggSchema& sch) {
    if (!rows_match(plan)) return "rows of another kind than the partials";
    for (const auto& s : plan.agg) {
        const dstd::String field = tier_field(plan, s.field);
        const dstd::String by = tier_field(plan, s.by);
        const auto refused = [&] {
            return "aggregate " + dstd::to_string(static_cast<int>(s.op)) +
                   " of field '" + s.field + "'";
        };
        if (!serves_op(s.op)) return refused();
        if (field.empty() != s.field.empty() || by.empty() != s.by.empty())
            return refused();
        // Only dur and size keep the values for quantiles and moments past two.
        if ((s.op == AggOp::Skew || s.op == AggOp::Kurt || s.op == AggOp::Pct ||
             s.op == AggOp::Hist) &&
            field != "dur" && field != "size")
            return refused();
        if (s.op == AggOp::Count && !field.empty()) return refused();
        // A partial knows the first and the last start of its events and the
        // last end.
        if (field == "ts" && s.op != AggOp::Min && s.op != AggOp::Max)
            return refused();
        if (field == "te" && s.op != AggOp::Max) return refused();
        // The scan scales ts, dur and te by time_scale and the partials hold
        // raw values.
        if (plan.time_scale != 1.0 &&
            (field == "ts" || field == "dur" || field == "te" || by == "ts" ||
             by == "dur" || by == "te"))
            return refused();
    }
    for (const auto& f : sch.fields)
        if (!answerable_field(tier_field(plan, f))) return "field '" + f + "'";
    return {};
}

bool wants_quantiles(const ViewPlan& plan) {
    for (const auto& s : plan.agg)
        if (s.op == AggOp::Pct || s.op == AggOp::Hist) return true;
    return false;
}

struct FilterSets {
    bool has_cat = false;
    bool has_name = false;
    bool has_host = false;
    bool has_pid = false;
    dstd::Vector<dstd::String> categories;
    dstd::Vector<dstd::String> names;
    dstd::Vector<dstd::String> hosts;
    dstd::Vector<std::uint64_t> pids;
};

bool add_leaf(const ViewPlan& plan, const duql::detail::FieldNode& field,
              const dstd::Vector<const duql::LiteralValue*>& values,
              FilterSets& sets) {
    if (field.any || values.empty()) return false;
    const dstd::String t = tier_field(plan, field.path);
    auto strings = [&](bool& seen, dstd::Vector<dstd::String>& out) {
        if (seen) return false;
        seen = true;
        for (const auto* v : values) {
            const auto* s = std::get_if<dstd::String>(v);
            if (!s) return false;
            out.emplace_back(s->data(), s->size());
        }
        return true;
    };
    if (t == "cat") return strings(sets.has_cat, sets.categories);
    if (t == "name") return strings(sets.has_name, sets.names);
    if (t == "hhash") return strings(sets.has_host, sets.hosts);
    if (t != "pid" || sets.has_pid) return false;
    sets.has_pid = true;
    for (const auto* v : values) {
        if (const auto* i = std::get_if<std::int64_t>(v)) {
            if (*i < 0) return false;
            sets.pids.push_back(static_cast<std::uint64_t>(*i));
        } else if (const auto* u = std::get_if<std::uint64_t>(v)) {
            sets.pids.push_back(*u);
        } else {
            return false;
        }
    }
    return true;
}

bool extract_sets(const ViewPlan& plan, const duql::detail::QueryNode& n,
                  FilterSets& sets) {
    using namespace duql;
    return std::visit(
        [&](const auto& node) -> bool {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, duql::detail::AndNode>) {
                return extract_sets(plan, *node.left, sets) &&
                       extract_sets(plan, *node.right, sets);
            } else if constexpr (std::is_same_v<T, duql::detail::CompareNode>) {
                if (node.op != CompareOp::Eq) return false;
                return add_leaf(plan, node.field, {&node.value.value}, sets);
            } else if constexpr (std::is_same_v<T, duql::detail::InNode>) {
                dstd::Vector<const LiteralValue*> values;
                for (const auto& e : node.values.elements)
                    values.push_back(&e.value);
                return add_leaf(plan, node.field, values, sets);
            } else {
                return false;
            }
        },
        n.data);
}

struct ValueCol {
    std::int32_t value_col;
    dstd::String field;
};

dstd::Vector<ValueCol> value_cols(
    const ViewPlan& plan, const dstd::Vector<dstd::String>& value_names) {
    dstd::Vector<ValueCol> out;
    for (std::size_t vc = 0; vc < value_names.size(); ++vc) {
        dstd::String base = agg_value_base_field(value_names[vc]);
        // The partial holds offset under its bare name.
        if (!plan_by_path(plan) &&
            base ==
                dstd::String(dftracer::utils::common::ARGS_PREFIX) + "offset")
            base = "offset";
        dstd::String field = tier_field(plan, base);
        if (!answerable_field(field)) continue;  // e.g. a SetUnion text column
        out.push_back({static_cast<std::int32_t>(vc), std::move(field)});
    }
    return out;
}

struct Prepared {
    AggQuery query;
    dstd::Vector<GroupKey> keys;
    dstd::Vector<dstd::String> extra;
    bool by_path = false;
};

void log_declined(const dstd::String& why) {
    DFTRACER_UTILS_LOG_DEBUG(
        "View: the index partials cannot answer this query ({}); the traces "
        "are scanned",
        why);
}

using Preparation = dstd::Result<dstd::Optional<Prepared>>;

Preparation declined(const dstd::String& why) {
    log_declined(why);
    return dstd::Optional<Prepared>();
}

enum class PrepSource { Partials, Trace };

// An empty result when the partials cannot answer `plan`. With Trace a file
// the index does not hold, a stale tier and an unreadable index leave the file
// to be read whole instead of declining the plan.
Preparation prepare(const ViewPlan& plan, bool typed, PrepSource from) {
    const bool trace = from == PrepSource::Trace;
    if (plan.files.empty()) return declined("no files");
    const auto past_pin = is_past_pin(plan);
    if (!past_pin.has_value()) return past_pin.error();
    if (*past_pin) return declined("an index past the view's pin");
    if (!plan.computed.empty()) return declined("computed columns");
    if (plan.time_range) return declined("a time range");
    if (plan.all_records) return declined("every record is asked for");
    if (plan.auto_numeric_metrics || !plan.numeric_arg_aggs.empty())
        return declined("aggregates over numeric args");
    if (plan.phase != Phase::Any && (typed || plan.phase != Phase::Events))
        return declined("a phase filter");
    const AggSchema& sch = ensure_schema(plan);
    if (plan.query && !query_answerable(plan))
        return declined("a filter the partials cannot evaluate");
    auto keys = partial_keys(plan);
    if (!keys) return declined("a group key the partials do not hold");
    if (const dstd::String why = unanswered_agg(plan, sch); !why.empty())
        return declined(why);

    Prepared p;
    p.by_path = plan_by_path(plan);
    p.keys = std::move(*keys);
    if (typed && plan.time_bucket_us > 0) {
        if (plan.bucket_origin_us != 0 || plan.bucket_origin_min ||
            plan.time_scale != 1.0)
            return declined("a shifted or scaled time bucket");
        // A path plan buckets on its time role, which the partials hold in
        // microseconds only when it maps to ts.
        if (p.by_path &&
            tier_field(plan, plan_record_schema(plan).roles.time) != "ts")
            return declined("a time bucket on a time role other than ts");
    } else if (plan.time_bucket_us != 0) {
        return declined("a time bucket on an untyped plan");
    }
    // A path-decoded trace is read by the scan's decoder, not the line parser
    // a re-read runs.
    if (p.by_path && trace) return declined("a path-decoded plan read whole");
    // The engine's quantile sketch moves with the order and size of the chunks
    // the values arrive in, so a fold would answer another quantile than the
    // frame path.
    if (trace && wants_quantiles(plan))
        return declined("a quantile aggregate of a scanned trace");
    if (p.by_path) {
        const auto complete = roles_complete(plan);
        if (!complete.has_value()) return complete.error();
        if (!*complete) return declined("a schema without every trace role");
    }

    AggQuery& q = p.query;
    std::uint64_t interval_us = q.config.time_interval_us;
    dstd::Vector<dstd::String> build_extra;
    // The first root with a config fixes the span of a pre-aggregated counter
    // and the extra keys. A root without one says nothing about either.
    bool config_seen = false;
    dstd::HashMap<dstd::String, std::size_t> root_of;
    dstd::Map<dstd::String, trace::index::Snapshot> snapshots;
    for (const auto& f : plan.files) {
        std::size_t at = q.sources.size();
        bool indexed = false;
        if (f.index_path.empty()) {
            if (!trace) return declined("a file with no index");
        } else {
            // The reason this file declines the plan, empty when it does not.
            auto reason = [&]() -> dstd::Result<const char*> {
                auto it = root_of.find(f.index_path);
                if (it == root_of.end()) {
                    auto snapshot = snapshot_of(plan, f.index_path);
                    if (!snapshot) return snapshot.error();
                    const auto config =
                        trace::index::schemas::dft::agg::tier::read_config(
                            *snapshot);
                    if (!config) return config.error();
                    if (!*config && !trace)
                        return "an index with no aggregation config";
                    AggQuerySource src;
                    src.index_path =
                        dstd::String(f.index_path.data(), f.index_path.size());
                    if (*config) {
                        src.params_hash = (*config)->params_hash;
                        if (!config_seen) {
                            config_seen = true;
                            interval_us = (*config)->time_interval_us;
                            build_extra = (*config)->extra_group_keys;
                        } else if (!trace &&
                                   (*config)->extra_group_keys != build_extra) {
                            // The sources keep their extra values under other
                            // keys.
                            return "indexes with other extra group keys";
                        }
                    }
                    src.snapshot =
                        dstd::make_shared<const trace::index::Snapshot>(
                            *snapshot);
                    it = root_of.emplace(f.index_path, q.sources.size()).first;
                    q.sources.push_back(std::move(src));
                    snapshots.emplace(f.index_path, std::move(*snapshot));
                }
                at = it->second;
                indexed = true;
                const trace::index::Snapshot& snapshot =
                    snapshots.at(f.index_path);
                const auto file = snapshot.file(get_logical_path(f.file_path));
                if (!file) {
                    if (!trace) return "a file missing from its index";
                } else if (!trace) {
                    if (snapshot.extension(
                            *file, trace::index::store::IndexExtension::Agg,
                            *q.sources[at].params_hash) !=
                        trace::index::Freshness::Current)
                        return "a file whose aggregation data is stale";
                }
                return static_cast<const char*>(nullptr);
            }();
            if (!reason) {
                // The scan reports an unreadable index.
                if (!trace)
                    return declined(dstd::fmt<"an unreadable index: {}">(
                        reason.error().message()));
                indexed = false;
            } else if (*reason) {
                return declined(*reason);
            }
        }
        if (!indexed) {
            // A file with no usable index is a source of its own.
            auto it = root_of.find(dstd::String());
            if (it == root_of.end()) {
                AggQuerySource src;
                it = root_of.emplace(dstd::String(), q.sources.size()).first;
                q.sources.push_back(std::move(src));
            }
            at = it->second;
        }
        q.sources[at].files.emplace_back(f.file_path.data(),
                                         f.file_path.size());
    }

    for (const auto& gk : p.keys) {
        switch (gk.kind) {
            case GroupKey::Kind::Pid:
                q.by_pid = true;
                break;
            case GroupKey::Kind::Tid:
                q.by_tid = true;
                break;
            case GroupKey::Kind::Fhash:
            case GroupKey::Kind::FilePath:
            case GroupKey::Kind::FileName:
                q.by_file = true;
                break;
            case GroupKey::Kind::Hhash:
            case GroupKey::Kind::HostName:
                q.by_host = true;
                break;
            case GroupKey::Kind::Arg:
                // The joined text of the kept values cannot tell a missing
                // one from an empty one, which the scan keeps apart, so a key
                // the build did not keep declines on a trace too.
                if (std::none_of(build_extra.begin(), build_extra.end(),
                                 [&](const dstd::String& e) {
                                     return dstd::StringView(e) ==
                                            dstd::StringView(gk.arg);
                                 }))
                    return declined(
                        "a group key on an arg the build did "
                        "not keep");
                q.by_extra = true;
                break;
            case GroupKey::Kind::Name:
            case GroupKey::Kind::Cat:
            case GroupKey::Kind::IoCat:
            case GroupKey::Kind::AccPat:
            case GroupKey::Kind::Rank:
            case GroupKey::Kind::Field:
            case GroupKey::Kind::Expr:
                break;
        }
    }
    q.only_file_keys =
        std::all_of(p.keys.begin(), p.keys.end(), [](const GroupKey& gk) {
            return gk.kind == GroupKey::Kind::Fhash ||
                   gk.kind == GroupKey::Kind::FilePath ||
                   gk.kind == GroupKey::Kind::FileName;
        });
    for (const auto& k : build_extra) p.extra.emplace_back(k.data(), k.size());
    q.config.time_interval_us = interval_us;
    q.config.extra_group_keys = std::move(build_extra);
    q.by_kind = true;
    q.quantiles = wants_quantiles(plan);
    // The dftracer line parser does not read a path-decoded file.
    q.reread = !p.by_path;
    q.from_trace = trace;
    if (typed) q.window_us = plan.time_bucket_us;

    if (plan.query) {
        FilterSets sets;
        if (extract_sets(plan, duql::detail::QueryAccess::root(*plan.query),
                         sets)) {
            q.categories = std::move(sets.categories);
            q.names = std::move(sets.names);
            q.hosts = std::move(sets.hosts);
            q.pids = std::move(sets.pids);
        } else if (p.by_path || plan.query->references("cat")) {
            // The partials compare a category lower-cased, which only a set
            // of equal values can say.
            return declined("a category filter that is not a set of values");
        } else {
            q.filter = *plan.query;
        }
    }
    return dstd::Optional<Prepared>(std::move(p));
}

// A path record's int field is signed where dftracer's is unsigned.
dataframe::FieldStat field_stat(
    const dstd::String& field,
    const trace::index::schemas::dft::agg::AggPartial& p,
    dataframe::FieldStatDomain domain) {
    dataframe::FieldStat fs;
    if (field == "dur") {
        fs = p.duration;
    } else if (field == "size") {
        fs = p.size;
    } else if (field == "offset") {
        fs = p.offset;
    } else if (field == "ts") {
        fs.n = p.count;
        fs.min = static_cast<double>(p.ts);
        fs.max = static_cast<double>(p.ts_last_start);
        fs.domain = domain;
        fs.esum = fs.emin = std::bit_cast<std::int64_t>(p.ts);
        fs.emax = std::bit_cast<std::int64_t>(p.ts_last_start);
    } else if (field == "te") {
        fs.n = p.count;
        fs.min = fs.max = static_cast<double>(p.te);
        fs.domain = domain;
        fs.esum = fs.emin = fs.emax = std::bit_cast<std::int64_t>(p.te);
    }
    if (domain == dataframe::FieldStatDomain::I64 &&
        fs.domain == dataframe::FieldStatDomain::U64)
        fs.domain = dataframe::FieldStatDomain::I64;
    return fs;
}

bool key_is_resolved(GroupKey::Kind kind) {
    return kind == GroupKey::Kind::FilePath ||
           kind == GroupKey::Kind::FileName ||
           kind == GroupKey::Kind::HostName || kind == GroupKey::Kind::Rank;
}

class Seeder {
   public:
    Seeder(const ViewPlan& plan, AggInputSpec spec,
           const dstd::Vector<GroupKey>& keys,
           const dstd::Vector<dstd::String>& extra, bool quantiles,
           const dstd::StringIntern& texts)
        : keys_(keys),
          extra_(extra),
          quantiles_(quantiles),
          texts_(texts),
          has_bucket_(plan.time_bucket_us > 0),
          domain_(plan_by_path(plan) ? dataframe::FieldStatDomain::I64
                                     : dataframe::FieldStatDomain::U64),
          spec_(std::move(spec)) {
        lowered_ = dataframe::lower_group_aggs(spec_.gaggs);
        cols_ = value_cols(plan, lowered_.value_names);
        sketches_.resize(cols_.size());
        // finalize_engine_result skips the resolver for a transformed key, so
        // it resolves at seed time. A plain resolved key resolves at finalize.
        for (const auto& gk : plan.group_by)
            if (gk.transform != GroupKey::Transform::None)
                resolver_ = ensure_resolver(plan);
    }

    std::size_t nkeys() const { return (has_bucket_ ? 1u : 0u) + keys_.size(); }

    dataframe::AggStatePtr new_state() const {
        auto state = dataframe::agg_new(lowered_.specs, spec_.dyn_specs);
        dataframe::agg_seed_begin(*state, nkeys());
        return state;
    }

    // A file key of a row without a file hash is null, as a scan keys it.
    void keys_of(const AggKey& key, dstd::Vector<dstd::String>& out,
                 dstd::Vector<bool>& nulls) const {
        out.clear();
        nulls.clear();
        if (has_bucket_) {
            out.emplace_back(dstd::to_string(key.window));
            nulls.push_back(false);
        }
        for (const auto& gk : keys_) {
            nulls.push_back(is_file_key(gk.kind) &&
                            key.file_text == dstd::StringIntern::NO_ID &&
                            key.file == 0);
            dstd::String v = group_value(gk, key);
            if (gk.transform != GroupKey::Transform::None) {
                if (resolver_ && key_is_resolved(gk.kind))
                    v = resolve_group_value(*resolver_, gk, v);
                v = apply_group_transform(gk, std::move(v));
            }
            out.emplace_back(v);
        }
    }

    void seed(dataframe::AggState& state,
              const dstd::Vector<dstd::String>& keys,
              const dstd::Vector<bool>& nulls,
              const trace::index::schemas::dft::agg::AggPartial& p) {
        dstd::Vector<dataframe::AggSeedValue> values;
        values.reserve(cols_.size());
        for (std::size_t i = 0; i < cols_.size(); ++i) {
            dataframe::AggSeedValue v;
            v.value_col = cols_[i].value_col;
            v.stat = field_stat(cols_[i].field, p, domain_);
            const trace::index::schemas::dft::agg::QuantileStore* q = nullptr;
            if (quantiles_) {
                if (cols_[i].field == "dur")
                    q = &p.duration_q;
                else if (cols_[i].field == "size")
                    q = &p.size_q;
            }
            if (q != nullptr && q->count() != 0) {
                sketches_[i].reset();
                q->add_to(sketches_[i]);
                v.sketch = &sketches_[i];
            }
            values.push_back(std::move(v));
        }
        dataframe::agg_seed_group(state, keys, nulls, p.count, values);
    }

   private:
    dstd::StringView text(std::uint32_t id) const {
        return id == dstd::StringIntern::NO_ID ? dstd::StringView{}
                                               : texts_.resolve(id);
    }

    static bool is_file_key(GroupKey::Kind kind) {
        return kind == GroupKey::Kind::Fhash ||
               kind == GroupKey::Kind::FilePath ||
               kind == GroupKey::Kind::FileName;
    }

    dstd::String group_value(const GroupKey& gk, const AggKey& k) const {
        switch (gk.kind) {
            case GroupKey::Kind::Name:
                return dstd::String(text(k.name));
            case GroupKey::Kind::Cat:
                return dstd::String(text(k.cat));
            case GroupKey::Kind::Pid:
                return dstd::to_string(k.pid);
            case GroupKey::Kind::Tid:
                return dstd::to_string(k.tid);
            case GroupKey::Kind::Fhash:
            case GroupKey::Kind::FilePath:
            case GroupKey::Kind::FileName: {
                if (k.file_text != dstd::StringIntern::NO_ID)
                    return dstd::String(text(k.file_text));
                if (k.file == 0) return {};
                char buf[::dftracer::utils::dstd::hash::HEX64_DIGITS];
                ::dftracer::utils::dstd::hash::format_hex64(k.file, buf);
                return dstd::String(buf, sizeof(buf));
            }
            case GroupKey::Kind::Hhash:
            case GroupKey::Kind::HostName:
                return dstd::String(text(k.host));
            case GroupKey::Kind::IoCat:
                return dstd::to_string(static_cast<int>(
                    trace::internal::io_category(text(k.name))));
            case GroupKey::Kind::AccPat:
                return "0";
            case GroupKey::Kind::Arg: {
                // The extra values are joined by 0x1F, one per key of the
                // query's extra_group_keys.
                dstd::StringView rest = text(k.extra);
                for (const auto& name : extra_) {
                    const std::size_t at = rest.find(EXTRA_SEP);
                    const dstd::StringView part = rest.substr(0, at);
                    if (name == gk.arg) return dstd::String(part);
                    if (at == dstd::StringView::npos) break;
                    rest.remove_prefix(at + 1);
                }
                return {};
            }
            case GroupKey::Kind::Rank:
            case GroupKey::Kind::Field:
            case GroupKey::Kind::Expr:
                // Never reached: partial_keys declines them.
                return {};
        }
        return {};
    }

    const dstd::Vector<GroupKey>& keys_;
    const dstd::Vector<dstd::String>& extra_;
    bool quantiles_;
    const dstd::StringIntern& texts_;
    bool has_bucket_;
    dataframe::FieldStatDomain domain_;
    AggInputSpec spec_;
    dataframe::LoweredGroupAggs lowered_;
    dstd::Vector<ValueCol> cols_;
    dstd::Vector<dataframe::DDSketch> sketches_;
    const dftracer::utils::trace::index::plan::GroupResolver* resolver_ =
        nullptr;
};

void log_answer(const AggQueryStats& st) {
    DFTRACER_UTILS_LOG_DEBUG(
        "View: the index partials answered over {} files: {} partials read, {} "
        "merged, {} pruned, {} split by time bins, {} re-read in {} chunks ({} "
        "events), {} unanswered",
        st.files, st.partials_read, st.partials_merged, st.partials_pruned,
        st.partials_binned, st.partials_reread, st.chunks_reread,
        static_cast<unsigned long long>(st.events_read), st.unanswered);
}

dataframe::AggStatePtr seed_rows_of(const ViewPlan& plan, AggInputSpec spec,
                                    const dstd::Vector<GroupKey>& keys,
                                    const dstd::Vector<dstd::String>& extra,
                                    bool quantiles,
                                    const dstd::StringIntern& texts,
                                    const AggQueryResult::Rows& rows) {
    Seeder seeder(plan, std::move(spec), keys, extra, quantiles, texts);
    auto state = seeder.new_state();
    dstd::Vector<dstd::String> texts_of_row;
    dstd::Vector<bool> nulls;
    const bool events_only = plan.phase == Phase::Events;
    for (const auto& [key, partial] : rows) {
        if (events_only && key.kind != AggKind::Event) continue;
        seeder.keys_of(key, texts_of_row, nulls);
        seeder.seed(*state, texts_of_row, nulls, partial);
    }
    dataframe::agg_seed_finalize(*state);
    return state;
}

dataframe::AggStatePtr seed_all(const ViewPlan& plan, AggInputSpec spec,
                                const Prepared& prep,
                                const AggQueryResult& result) {
    return seed_rows_of(plan, std::move(spec), prep.keys, prep.extra,
                        prep.query.quantiles, result.strings(), result.rows);
}

}  // namespace

dstd::Result<dstd::Optional<AggQuery>> trace_fold_query(const ViewPlan& plan) {
    auto prep = prepare(plan, /*typed=*/false, PrepSource::Trace);
    if (!prep) return prep.error();
    if (!*prep) return dstd::Optional<AggQuery>();
    (*prep)->query.cancelled = plan.cancelled;
    return dstd::Optional<AggQuery>(std::move((*prep)->query));
}

dataframe::AggStatePtr seed_rows(const ViewPlan& plan, AggInputSpec spec,
                                 const AggQuery& query,
                                 const dstd::StringIntern& texts,
                                 const AggQueryResult::Rows& rows) {
    auto keys = partial_keys(plan);
    dstd::Vector<dstd::String> extra;
    for (const auto& k : query.config.extra_group_keys) extra.emplace_back(k);
    return seed_rows_of(plan, std::move(spec),
                        keys ? *keys : dstd::Vector<GroupKey>{}, extra,
                        query.quantiles, texts, rows);
}

coro::CoroTask<dstd::Result<bool>> partials_collect(
    const ViewPlan& plan, dataframe::AggStatePtr& out) {
    auto prepared = prepare(plan, /*typed=*/false, PrepSource::Partials);
    if (!prepared) co_return prepared.error();
    if (!*prepared) co_return false;
    Prepared& prep = **prepared;
    auto spec = make_agg_input_spec(plan);
    if (!spec) co_return spec.error();
    auto ran = co_await run_agg_query(prep.query);
    if (!ran) co_return ran.error();
    AggQueryResult result = std::move(*ran);
    log_answer(result.stats);
    if (result.stats.unanswered != 0) {
        log_declined(
            "partials that only a re-read could answer, which the "
            "query may not do");
        co_return false;
    }
    out = seed_all(plan, std::move(*spec), prep, result);
    co_return true;
}

coro::CoroTask<dstd::Result<bool>> partials_scan_collect(
    const ViewPlan& plan, dataframe::AggStatePtr& out) {
    auto prepared = prepare(plan, /*typed=*/false, PrepSource::Trace);
    if (!prepared) co_return prepared.error();
    if (!*prepared) co_return false;
    Prepared& prep = **prepared;
    prep.query.cancelled = plan.cancelled;
    auto spec = make_agg_input_spec(plan);
    if (!spec) co_return spec.error();
    auto ran = co_await run_agg_query(prep.query);
    if (!ran) co_return ran.error();
    AggQueryResult result = std::move(*ran);
    DFTRACER_UTILS_LOG_DEBUG(
        "View: the trace answered over {} files: {} events in {} chunks",
        result.stats.files,
        static_cast<unsigned long long>(result.stats.events_read),
        result.stats.chunks_reread);
    out = seed_all(plan, std::move(*spec), prep, result);
    co_return true;
}

coro::CoroTask<dstd::Result<bool>> partials_collect_typed(
    const ViewPlan& plan, dataframe::DataFrame& out_regular,
    dataframe::DataFrame& out_aggregated, int shard_begin, int shard_end,
    const ProgressFn* progress) {
    auto prepared = prepare(plan, /*typed=*/true, PrepSource::Partials);
    if (!prepared) co_return prepared.error();
    if (!*prepared) co_return false;
    Prepared& prep = **prepared;
    const int shards = trace::index::schemas::dft::agg::AGG_KEY_NUM_SHARDS;
    if (shard_end <= 0) shard_end = shards;
    const bool whole = shard_begin <= 0 && shard_end >= shards;
    const auto total = static_cast<std::size_t>(shard_end - shard_begin);
    if (progress && *progress) (*progress)(0, total);

    auto spec = make_agg_input_spec(plan);
    if (!spec) co_return spec.error();
    auto ran = co_await run_agg_query(prep.query);
    if (!ran) co_return ran.error();
    AggQueryResult result = std::move(*ran);
    log_answer(result.stats);
    if (result.stats.unanswered != 0) {
        log_declined(
            "partials that only a re-read could answer, which the "
            "query may not do");
        co_return false;
    }

    Seeder seeder(plan, std::move(*spec), prep.keys, prep.extra,
                  prep.query.quantiles, result.strings());
    auto events = seeder.new_state();
    auto profiles = seeder.new_state();
    dstd::Vector<dstd::String> keys;
    dstd::Vector<bool> nulls;
    dstd::String joined;
    for (const auto& [key, partial] : result.rows) {
        if (key.kind == AggKind::System) continue;
        seeder.keys_of(key, keys, nulls);
        if (!whole) {
            joined.clear();
            for (const auto& k : keys) {
                joined += k;
                joined += GROUP_SEP;
            }
            const auto shard =
                static_cast<int>(dftracer::utils::dstd::hash::xxh3_64(joined) %
                                 static_cast<std::uint64_t>(shards));
            if (shard < shard_begin || shard >= shard_end) continue;
        }
        seeder.seed(key.kind == AggKind::Event ? *events : *profiles, keys,
                    nulls, partial);
    }
    dataframe::agg_seed_finalize(*events);
    dataframe::agg_seed_finalize(*profiles);
    auto regular = finalize_engine_result(*events, plan);
    if (!regular) co_return regular.error();
    auto aggregated = finalize_engine_result(*profiles, plan);
    if (!aggregated) co_return aggregated.error();
    out_regular = std::move(*regular);
    out_aggregated = std::move(*aggregated);
    if (progress && *progress) (*progress)(total, total);
    co_return true;
}

}  // namespace dftracer::utils::trace::views::detail
