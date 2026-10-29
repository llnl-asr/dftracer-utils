#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/hash/hex64.h>
#include <dftracer/utils/duql/ast.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/duql/term.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/plan/view_resolver.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_agg_partials.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_scan.h>

#include <algorithm>
#include <bit>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace dftracer::utils::trace::views::detail {

namespace {

namespace agg = dftracer::utils::index::schemas::dft::agg;
namespace dataframe = dftracer::utils::dataframe;
namespace store = dftracer::utils::index::store;
using store::IndexDatabase;
using store::internal::get_logical_path;

constexpr char EXTRA_SEP = '\x1f';

// The tier's name for field `f` of `plan`: a dftracer field keeps its own. A
// path-decoded record's name, entity, lane, duration or time role maps to
// name, pid, tid, dur or ts when the tier holds exactly the value the scan
// reads: a required field, a string name, an int otherwise and times in
// microseconds. Empty when the tier holds no such value.
std::string tier_field(const ViewPlan& plan, std::string_view f) {
    if (!plan_by_path(plan)) return std::string(f);
    namespace ix = dftracer::utils::index;
    const ix::RecordSchema& s = plan_record_schema(plan);
    const ix::Roles& r = s.roles;
    auto exact = [&](const std::string& path, ix::FieldType type) {
        if (f.empty() || path != f) return false;
        const ix::FieldSpec* spec = s.field_at(path);
        return spec && !spec->optional && spec->type == type;
    };
    if (exact(r.name, ix::FieldType::String)) return "name";
    if (exact(r.entity, ix::FieldType::Int)) return "pid";
    if (exact(r.lane, ix::FieldType::Int)) return "tid";
    if (r.duration_unit == ix::TimeUnit::Us &&
        exact(r.duration, ix::FieldType::Int))
        return "dur";
    if (r.time_unit == ix::TimeUnit::Us && exact(r.time, ix::FieldType::Int))
        return "ts";
    return {};
}

// Whether every record of `plan`'s path-decoded files holds a value of each
// role the tier maps, not negative: the tier folds a missing value as 0 where
// the scan reads null. Each chunk's zone must show the role in every record.
bool roles_complete(const ViewPlan& plan) {
    namespace ix = dftracer::utils::index;
    const ix::Roles& r = plan_record_schema(plan).roles;
    std::vector<std::string_view> paths;
    for (const std::string* p :
         {&r.name, &r.entity, &r.lane, &r.duration, &r.time})
        if (!tier_field(plan, *p).empty()) paths.push_back(*p);
    if (paths.empty()) return true;
    for (const auto& f : plan.files) {
        IndexDatabase db(f.index_path, store::IndexOpenMode::ReadOnly);
        const int fid = db.get_file_info_id(get_logical_path(f.file_path));
        if (fid < 0 ||
            !db.extension_current(fid, store::IndexExtension::Zonemap))
            return false;
        const auto spans = db.query_chunk_spans(fid);
        for (std::string_view p : paths) {
            std::map<std::uint64_t,
                     dftracer::utils::index::extensions::kinds::Zone>
                zones;
            for (const auto& [granule, bytes] : db.path_granules(
                     fid, store::IndexExtension::Zonemap, std::string(p)))
                if (auto z =
                        dftracer::utils::index::extensions::kinds::decode_zone(
                            bytes))
                    zones.emplace(granule, std::move(*z));
            for (std::uint64_t i = 0; i < spans.size(); ++i) {
                if (spans[i].first_line_num == 0 ||
                    spans[i].last_line_num < spans[i].first_line_num)
                    continue;
                const std::uint64_t lines =
                    spans[i].last_line_num - spans[i].first_line_num + 1;
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

// Fields the partials answer: ts is only its min, te only its max, so those
// ops are gated in unanswered_agg.
bool answerable_field(const std::string& f) {
    return f == "dur" || f == "size" || f == "offset" || f == "ts" || f == "te";
}

// Whether the partials hold the rows the scan of `plan` reads: they hold every
// record, and a path schema's `data` row set can leave some out.
bool rows_match(const ViewPlan& plan) {
    return !plan_by_path(plan) || plan.all_records ||
           plan_record_schema(plan).data.empty();
}

// The group keys the partials answer for `plan`; a path plan's field keys on
// the name, entity and lane roles become name, pid and tid keys. nullopt when
// a key is one they cannot answer.
std::optional<std::vector<GroupKey>> partial_keys(const ViewPlan& plan) {
    using Kind = GroupKey::Kind;
    std::vector<GroupKey> keys = plan.group_by;
    if (plan_by_path(plan)) {
        for (auto& gk : keys) {
            if (gk.kind != Kind::Field) return std::nullopt;
            const std::string t = tier_field(plan, gk.arg);
            if (t == "name")
                gk.kind = Kind::Name;
            else if (t == "pid")
                gk.kind = Kind::Pid;
            else if (t == "tid")
                gk.kind = Kind::Tid;
            else
                return std::nullopt;
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
                return std::nullopt;
        }
    }
    return keys;
}

bool reads_lookup(const duql::QueryNode& node) {
    return std::visit(
        [](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::AndNode> ||
                          std::is_same_v<T, duql::OrNode>) {
                return reads_lookup(*n.left) || reads_lookup(*n.right);
            } else if constexpr (std::is_same_v<T, duql::NotNode>) {
                return reads_lookup(*n.operand);
            } else if constexpr (std::is_same_v<T, duql::ExprLeaf>) {
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

// Filter fields the partials answer: the ones a group key or a set of the
// query names.
bool query_answerable(const ViewPlan& plan) {
    const duql::Query& q = *plan.query;
    // Aggregated records keep no array elements.
    if (!q.any_paths().empty()) return false;
    // The view binds a row set's rows only on the scan path.
    if (reads_lookup(q.root())) return false;
    for (std::string_view f : q.fields()) {
        const std::string t = tier_field(plan, f);
        if (t != "name" && t != "pid" && t != "tid" && t != "fhash" &&
            t != "hhash" && t != "cat")
            return false;
    }
    return true;
}

// Ops the seed path can reconstruct from a partial's FieldStat and quantile
// store. Occupancy and ArgMax/SetUnion keep no such state, so they decline to
// the scan. No default: -Wswitch flags a newly added op.
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

// The aggregate or field of `plan` the partials cannot answer, as a phrase
// for the decline log; empty when they answer every one.
std::string unanswered_agg(const ViewPlan& plan, const AggSchema& sch) {
    if (!rows_match(plan)) return "rows of another kind than the partials";
    for (const auto& s : plan.agg) {
        const std::string field = tier_field(plan, s.field);
        const std::string by = tier_field(plan, s.by);
        const auto refused = [&] {
            return "aggregate " + std::to_string(static_cast<int>(s.op)) +
                   " of field '" + s.field + "'";
        };
        if (!serves_op(s.op)) return refused();
        if (field.empty() != s.field.empty() || by.empty() != s.by.empty())
            return refused();
        // Only dur and size keep their values for quantiles and moments past
        // two; skew, kurtosis, percentiles and histograms on any other field
        // must scan.
        if ((s.op == AggOp::Skew || s.op == AggOp::Kurt || s.op == AggOp::Pct ||
             s.op == AggOp::Hist) &&
            field != "dur" && field != "size")
            return refused();
        if (s.op == AggOp::Count && !field.empty()) return refused();
        // A partial knows the first and the last start of its events, and
        // the last end.
        if (field == "ts" && s.op != AggOp::Min && s.op != AggOp::Max)
            return refused();
        if (field == "te" && s.op != AggOp::Max) return refused();
        // The scan scales ts/dur/te by time_scale; the partials hold raw
        // values, so a scaled-field agg would diverge. Decline it.
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

// The category, name, host and pid sets of a filter that is a conjunction of
// equality and membership leaves on them, each field once.
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

bool add_leaf(const ViewPlan& plan, const duql::FieldNode& field,
              const std::vector<const duql::LiteralValue*>& values,
              FilterSets& sets) {
    if (field.any || values.empty()) return false;
    const std::string t = tier_field(plan, field.path);
    auto strings = [&](bool& seen, dstd::Vector<dstd::String>& out) {
        if (seen) return false;
        seen = true;
        for (const auto* v : values) {
            const auto* s = std::get_if<std::string>(v);
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

bool extract_sets(const ViewPlan& plan, const duql::QueryNode& n,
                  FilterSets& sets) {
    using namespace duql;
    return std::visit(
        [&](const auto& node) -> bool {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndNode>) {
                return extract_sets(plan, *node.left, sets) &&
                       extract_sets(plan, *node.right, sets);
            } else if constexpr (std::is_same_v<T, CompareNode>) {
                if (node.op != CompareOp::Eq) return false;
                return add_leaf(plan, node.field, {&node.value.value}, sets);
            } else if constexpr (std::is_same_v<T, InNode>) {
                std::vector<const LiteralValue*> values;
                for (const auto& e : node.values.elements)
                    values.push_back(&e.value);
                return add_leaf(plan, node.field, values, sets);
            } else {
                return false;
            }
        },
        n.data);
}

// A value column the partials fill: its AggState value_col index and the base
// field (dur/size/offset/ts/te) whose stat feeds it.
struct ValueCol {
    std::int32_t value_col;
    std::string field;
};

std::vector<ValueCol> value_cols(const ViewPlan& plan,
                                 const std::vector<std::string>& value_names) {
    std::vector<ValueCol> out;
    for (std::size_t vc = 0; vc < value_names.size(); ++vc) {
        std::string base = agg_value_base_field(value_names[vc]);
        // offset is an arg, so its value column is named args.offset; the
        // partial holds it under the bare name.
        if (!plan_by_path(plan) &&
            base == std::string(dftracer::utils::ARGS_PREFIX) + "offset")
            base = "offset";
        std::string field = tier_field(plan, base);
        if (!answerable_field(field)) continue;  // e.g. a SetUnion text column
        out.push_back({static_cast<std::int32_t>(vc), std::move(field)});
    }
    return out;
}

// The query for `plan`, the keys it groups by, and what the rows are read
// with.
struct Prepared {
    AggQuery query;
    std::vector<GroupKey> keys;
    std::vector<std::string> extra;
    bool by_path = false;
};

// Says, at debug level, why the partials cannot answer a plan: the view then
// scans every trace file it reads.
std::nullopt_t declined(const std::string& why) {
    DFTRACER_UTILS_LOG_DEBUG(
        "View: the index partials cannot answer this query (%s); the traces "
        "are scanned",
        why.c_str());
    return std::nullopt;
}

// nullopt when the partials cannot answer `plan`; see partials_collect.
std::optional<Prepared> prepare(const ViewPlan& plan, bool typed) {
    if (plan.files.empty()) return declined("no files");
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
    if (const std::string why = unanswered_agg(plan, sch); !why.empty())
        return declined(why);

    Prepared p;
    p.by_path = plan_by_path(plan);
    p.keys = std::move(*keys);
    if (typed && plan.time_bucket_us > 0) {
        // The partials are keyed from time 0 in the trace's own unit.
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
    if (p.by_path && !roles_complete(plan))
        return declined("a schema without every trace role");

    AggQuery& q = p.query;
    std::uint64_t interval_us = 0;
    try {
        std::map<std::string, std::size_t> root_of;
        std::map<std::string, std::unique_ptr<IndexDatabase>> dbs;
        for (const auto& f : plan.files) {
            if (f.index_path.empty()) return declined("a file with no index");
            auto it = root_of.find(f.index_path);
            if (it == root_of.end()) {
                auto db = std::make_unique<IndexDatabase>(
                    f.index_path, store::IndexOpenMode::ReadOnly);
                const auto config = agg::tier::read_config(*db);
                if (!config)
                    return declined("an index with no aggregation config");
                AggQuerySource src;
                src.index_path =
                    dstd::String(f.index_path.data(), f.index_path.size());
                src.params_hash = config->params_hash;
                if (q.sources.empty()) {
                    interval_us = config->time_interval_us;
                    p.extra = config->extra_group_keys;
                } else if (config->extra_group_keys != p.extra) {
                    // The sources keep their extra values under other keys.
                    return declined("indexes with other extra group keys");
                }
                it = root_of.emplace(f.index_path, q.sources.size()).first;
                q.sources.push_back(std::move(src));
                dbs.emplace(f.index_path, std::move(db));
            }
            const IndexDatabase& db = *dbs.at(f.index_path);
            const int fid = db.get_file_info_id(get_logical_path(f.file_path));
            if (fid < 0) return declined("a file missing from its index");
            const auto state =
                db.extension_state(fid, store::IndexExtension::Agg);
            if (!state || !state->ready ||
                state->version !=
                    store::layout::ext_version(store::layout::Ext::Agg) ||
                state->params_hash != *q.sources[it->second].params_hash)
                return declined("a file whose aggregation data is stale");
            q.sources[it->second].files.emplace_back(f.file_path.data(),
                                                     f.file_path.size());
        }
    } catch (const std::exception& e) {
        // An index that cannot be read cannot be answered from; the scan
        // reports it.
        return declined(std::string("an unreadable index: ") + e.what());
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
                // Only a key the build kept the values of is in the partials.
                if (std::find(p.extra.begin(), p.extra.end(), gk.arg) ==
                    p.extra.end())
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
    q.config.time_interval_us = interval_us;
    for (const auto& k : p.extra) q.config.extra_group_keys.push_back(k);
    q.by_kind = true;
    q.quantiles = wants_quantiles(plan);
    // A path-decoded file is not read by the dftracer line parser, so what a
    // re-read would answer is left to the scan.
    q.reread = !p.by_path;
    if (typed) q.window_us = plan.time_bucket_us;

    if (plan.query) {
        FilterSets sets;
        if (extract_sets(plan, plan.query->root(), sets)) {
            q.categories = std::move(sets.categories);
            q.names = std::move(sets.names);
            q.hosts = std::move(sets.hosts);
            q.pids = std::move(sets.pids);
        } else if (p.by_path || plan.query->references("cat")) {
            // A category compares lower-cased in the partials, which only a
            // set of equal values can say.
            return declined("a category filter that is not a set of values");
        } else {
            q.filter = *plan.query;
        }
    }
    return p;
}

// The stat of `field` of a partial, in the domain the scan reads: a path
// record's int field is signed where dftracer's is unsigned.
dataframe::FieldStat field_stat(const std::string& field,
                                const agg::AggPartial& p,
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

// Whether a group key relabels an opaque hash to a name after the
// aggregation (finalize_engine_result resolves these).
bool key_is_resolved(GroupKey::Kind kind) {
    return kind == GroupKey::Kind::FilePath ||
           kind == GroupKey::Kind::FileName ||
           kind == GroupKey::Kind::HostName || kind == GroupKey::Kind::Rank;
}

// What the rows of a result seed into engine states with.
class Seeder {
   public:
    Seeder(const ViewPlan& plan, const Prepared& prep,
           const AggQueryResult& result)
        : prep_(prep),
          res_(result),
          has_bucket_(plan.time_bucket_us > 0),
          domain_(plan_by_path(plan) ? dataframe::FieldStatDomain::I64
                                     : dataframe::FieldStatDomain::U64) {
        spec_ = make_agg_input_spec(plan);
        lowered_ = dataframe::lower_group_aggs(spec_.gaggs);
        cols_ = value_cols(plan, lowered_.value_names);
        sketches_.resize(cols_.size());
        // A transformed key needs the resolved and transformed value at seed
        // time (finalize_engine_result skips the resolver for it); a plain
        // resolved key is seeded as its raw hash and resolved at finalize.
        for (const auto& gk : plan.group_by)
            if (gk.transform != GroupKey::Transform::None)
                resolver_ = ensure_resolver(plan);
    }

    std::size_t nkeys() const {
        return (has_bucket_ ? 1u : 0u) + prep_.keys.size();
    }

    dataframe::AggStatePtr new_state() const {
        auto state = dataframe::agg_new(lowered_.specs, spec_.dyn_specs);
        dataframe::agg_seed_begin(*state, nkeys());
        return state;
    }

    // The key texts of a row, [time_bucket?, group_by...], and which of them
    // are null: a file key of a row without a file hash, as a scan keys it.
    void keys_of(const AggKey& key, std::vector<std::string>& out,
                 std::vector<bool>& nulls) const {
        out.clear();
        nulls.clear();
        if (has_bucket_) {
            out.push_back(std::to_string(key.window));
            nulls.push_back(false);
        }
        for (const auto& gk : prep_.keys) {
            nulls.push_back(is_file_key(gk.kind) &&
                            key.file_text == dstd::StringIntern::NO_ID &&
                            key.file == 0);
            std::string v = group_value(gk, key);
            if (gk.transform != GroupKey::Transform::None) {
                if (resolver_ && key_is_resolved(gk.kind))
                    v = resolve_group_value(*resolver_, gk, v);
                v = apply_group_transform(gk, std::move(v));
            }
            out.push_back(std::move(v));
        }
    }

    void seed(dataframe::AggState& state, const std::vector<std::string>& keys,
              const std::vector<bool>& nulls, const agg::AggPartial& p) {
        std::vector<dataframe::AggSeedValue> values;
        values.reserve(cols_.size());
        for (std::size_t i = 0; i < cols_.size(); ++i) {
            dataframe::AggSeedValue v;
            v.value_col = cols_[i].value_col;
            v.stat = field_stat(cols_[i].field, p, domain_);
            const agg::QuantileStore* q = nullptr;
            if (prep_.query.quantiles) {
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
    static bool is_file_key(GroupKey::Kind kind) {
        return kind == GroupKey::Kind::Fhash ||
               kind == GroupKey::Kind::FilePath ||
               kind == GroupKey::Kind::FileName;
    }

    std::string group_value(const GroupKey& gk, const AggKey& k) const {
        switch (gk.kind) {
            case GroupKey::Kind::Name:
                return std::string(res_.text(k.name));
            case GroupKey::Kind::Cat:
                return std::string(res_.text(k.cat));
            case GroupKey::Kind::Pid:
                return std::to_string(k.pid);
            case GroupKey::Kind::Tid:
                return std::to_string(k.tid);
            case GroupKey::Kind::Fhash:
            case GroupKey::Kind::FilePath:
            case GroupKey::Kind::FileName: {
                if (k.file_text != dstd::StringIntern::NO_ID)
                    return std::string(res_.text(k.file_text));
                if (k.file == 0) return {};
                char buf[::dftracer::utils::hash::HEX64_DIGITS];
                ::dftracer::utils::hash::format_hex64(k.file, buf);
                return std::string(buf, sizeof(buf));
            }
            case GroupKey::Kind::Hhash:
            case GroupKey::Kind::HostName:
                return std::string(res_.text(k.host));
            case GroupKey::Kind::IoCat:
                return std::to_string(static_cast<int>(
                    trace::internal::io_category(res_.text(k.name))));
            case GroupKey::Kind::AccPat:
                return "0";
            case GroupKey::Kind::Arg: {
                // The extra values are joined by 0x1F, one per key of the
                // query's extra_group_keys.
                std::string_view rest = res_.text(k.extra);
                for (const auto& name : prep_.extra) {
                    const std::size_t at = rest.find(EXTRA_SEP);
                    const std::string_view part = rest.substr(0, at);
                    if (name == gk.arg) return std::string(part);
                    if (at == std::string_view::npos) break;
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

    const Prepared& prep_;
    const AggQueryResult& res_;
    bool has_bucket_;
    dataframe::FieldStatDomain domain_;
    AggInputSpec spec_;
    dataframe::LoweredGroupAggs lowered_;
    std::vector<ValueCol> cols_;
    std::vector<dataframe::DDSketch> sketches_;
    const dftracer::utils::index::plan::GroupResolver* resolver_ = nullptr;
};

AggQueryResult run_or_throw(Result<AggQueryResult>&& r) {
    if (!r) throw DFTUtilsException(r.error());
    return std::move(*r);
}

// Says, at debug level, how the partials answered a query.
void log_answer(const AggQueryStats& st) {
    DFTRACER_UTILS_LOG_DEBUG(
        "View: the index partials answered over %zu files: %zu partials read, "
        "%zu merged, %zu pruned, %zu split by time bins, %zu re-read in %zu "
        "chunks (%zu from their column blocks, %llu events), %zu unanswered",
        st.files, st.partials_read, st.partials_merged, st.partials_pruned,
        st.partials_binned, st.partials_reread, st.chunks_reread,
        st.chunks_from_columns, static_cast<unsigned long long>(st.events_read),
        st.unanswered);
}

}  // namespace

coro::CoroTask<bool> partials_collect(const ViewPlan& plan,
                                      dataframe::AggStatePtr& out) {
    auto prep = prepare(plan, /*typed=*/false);
    if (!prep) co_return false;
    auto ran = co_await run_agg_query(prep->query);
    AggQueryResult result = run_or_throw(std::move(ran));
    log_answer(result.stats);
    if (result.stats.unanswered != 0) {
        declined(
            "partials that only a re-read could answer, which the "
            "query may not do");
        co_return false;
    }

    Seeder seeder(plan, *prep, result);
    auto state = seeder.new_state();
    std::vector<std::string> keys;
    std::vector<bool> nulls;
    const bool events_only = plan.phase == Phase::Events;
    for (const auto& [key, partial] : result.rows) {
        if (events_only && key.kind != AggKind::Event) continue;
        seeder.keys_of(key, keys, nulls);
        seeder.seed(*state, keys, nulls, partial);
    }
    dataframe::agg_seed_finalize(*state);
    out = std::move(state);
    co_return true;
}

coro::CoroTask<bool> partials_collect_typed(
    const ViewPlan& plan, dataframe::DataFrame& out_regular,
    dataframe::DataFrame& out_aggregated, int shard_begin, int shard_end,
    const ProgressFn* progress) {
    auto prep = prepare(plan, /*typed=*/true);
    if (!prep) co_return false;
    const int shards = agg::AGG_KEY_NUM_SHARDS;
    if (shard_end <= 0) shard_end = shards;
    const bool whole = shard_begin <= 0 && shard_end >= shards;
    const auto total = static_cast<std::size_t>(shard_end - shard_begin);
    if (progress && *progress) (*progress)(0, total);

    auto ran = co_await run_agg_query(prep->query);
    AggQueryResult result = run_or_throw(std::move(ran));
    log_answer(result.stats);
    if (result.stats.unanswered != 0) {
        declined(
            "partials that only a re-read could answer, which the "
            "query may not do");
        co_return false;
    }

    Seeder seeder(plan, *prep, result);
    auto events = seeder.new_state();
    auto profiles = seeder.new_state();
    std::vector<std::string> keys;
    std::vector<bool> nulls;
    std::string joined;
    for (const auto& [key, partial] : result.rows) {
        if (key.kind == AggKind::System) continue;
        seeder.keys_of(key, keys, nulls);
        if (!whole) {
            joined.clear();
            for (const auto& k : keys) {
                joined += k;
                joined += GROUP_SEP;
            }
            const auto shard = static_cast<int>(
                dftracer::utils::hash::fnv1a_mix(
                    dftracer::utils::hash::fnv1a_hash(joined)) %
                static_cast<std::uint64_t>(shards));
            if (shard < shard_begin || shard >= shard_end) continue;
        }
        seeder.seed(key.kind == AggKind::Event ? *events : *profiles, keys,
                    nulls, partial);
    }
    dataframe::agg_seed_finalize(*events);
    dataframe::agg_seed_finalize(*profiles);
    out_regular = finalize_engine_result(*events, plan);
    out_aggregated = finalize_engine_result(*profiles, plan);
    if (progress && *progress) (*progress)(total, total);
    co_return true;
}

}  // namespace dftracer::utils::trace::views::detail
