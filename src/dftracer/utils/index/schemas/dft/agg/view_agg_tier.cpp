#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/schemas/dft/agg/view_agg_tier.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/no_destructor.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_db_open.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_output.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_serialization.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics_serialization.h>
#include <dftracer/utils/index/store/column_families.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/query/ast.h>
#include <dftracer/utils/query/evaluator.h>
#include <dftracer/utils/query/pattern.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <cctype>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace dataframe = dftracer::utils::dataframe;

namespace dftracer::utils::index::schemas::dft::agg {

namespace {

namespace agg = index::schemas::dft::agg;
using agg::AggKeyView;
using agg::AggMetricsFullView;
using index::store::IndexDatabase;
using index::store::internal::get_logical_path;

// Fields the tier's stored MetricStats can answer. ts is only its min, te only
// its max, so those ops are gated in answerable().
bool answerable_field(const std::string& f) {
    return f == "dur" || f == "size" || f == "offset" || f == "ts" || f == "te";
}

std::string to_lower_ascii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void lower_literal(query::LiteralNode& lit) {
    if (auto* s = std::get_if<std::string>(&lit.value)) *s = to_lower_ascii(*s);
}

// A copy of the query AST with `cat` equality/membership literals lowercased.
// The tier stores cat lowercased (the aggregation's canonical form), so a
// filter literal like "POSIX" is folded to match; this also restores the
// pre-View iter_arrow behavior of comparing cat case-insensitively.
query::QueryNodePtr clone_cat_lowered(const query::QueryNode& n) {
    using namespace query;
    return std::visit(
        [](const auto& node) -> QueryNodePtr {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, CompareNode>) {
                CompareNode c = node;
                if (c.field.path == "cat") lower_literal(c.value);
                return make_node(std::move(c));
            } else if constexpr (std::is_same_v<T, InNode>) {
                InNode c = node;
                if (c.field.path == "cat") {
                    for (auto& e : c.values.elements) lower_literal(e);
                    c.values.set = make_in_set(c.values);
                }
                return make_node(std::move(c));
            } else if constexpr (std::is_same_v<T, NotInNode>) {
                NotInNode c = node;
                if (c.field.path == "cat") {
                    for (auto& e : c.values.elements) lower_literal(e);
                    c.values.set = make_in_set(c.values);
                }
                return make_node(std::move(c));
            } else if constexpr (std::is_same_v<T, AndNode>) {
                return make_node(AndNode{clone_cat_lowered(*node.left),
                                         clone_cat_lowered(*node.right)});
            } else if constexpr (std::is_same_v<T, OrNode>) {
                return make_node(OrNode{clone_cat_lowered(*node.left),
                                        clone_cat_lowered(*node.right)});
            } else if constexpr (std::is_same_v<T, NotNode>) {
                return make_node(NotNode{clone_cat_lowered(*node.operand)});
            } else {
                return make_node(T(node));
            }
        },
        n.data);
}

// A cat predicate is foldable onto the tier's lowercased cat only when it is an
// equality/membership form (whose literal we can lowercase). A like/regex on
// cat can't be case-folded safely, so the tier declines and the scan (raw cat)
// answers it.
bool cat_foldable(const query::QueryNode& n) {
    using namespace query;
    return std::visit(
        [](const auto& node) -> bool {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, MatchNode>)
                return node.field.path != "cat";
            else if constexpr (std::is_same_v<T, AndNode> ||
                               std::is_same_v<T, OrNode>)
                return cat_foldable(*node.left) && cat_foldable(*node.right);
            else if constexpr (std::is_same_v<T, NotNode>)
                return cat_foldable(*node.operand);
            else
                return true;
        },
        n.data);
}

// Filter fields evaluable from a CF key. cat is answerable only when foldable
// (see cat_foldable): the tier stores it lowercased, so equality/membership
// literals are lowercased to match, but like/regex on cat must scan.
bool query_answerable(const query::Query& q) {
    // Aggregated records keep no array elements.
    if (!q.any_paths().empty()) return false;
    for (std::string_view f : q.fields())
        if (f != "name" && f != "pid" && f != "tid" && f != "fhash" &&
            f != "hhash" && f != "cat")
            return false;
    return cat_foldable(q.root());
}

// Ops the seed path can reconstruct from stored FieldStat + optional DDSketch.
// Occupancy and ArgMax/SetUnion keep no such state, so they decline to the
// scan. No default: -Wswitch flags a newly added op.
bool tier_serves_op(trace::views::AggOp op) {
    switch (op) {
        case trace::views::AggOp::Count:
        case trace::views::AggOp::Sum:
        case trace::views::AggOp::Min:
        case trace::views::AggOp::Max:
        case trace::views::AggOp::Mean:
        case trace::views::AggOp::Var:
        case trace::views::AggOp::Std:
        case trace::views::AggOp::Skew:
        case trace::views::AggOp::Kurt:
        case trace::views::AggOp::SumSq:
        case trace::views::AggOp::Pct:
        case trace::views::AggOp::Hist:
            return true;
        case trace::views::AggOp::ArgMax:
        case trace::views::AggOp::SetUnion:
        case trace::views::AggOp::Busy:
        case trace::views::AggOp::Concurrency:
        case trace::views::AggOp::Utilization:
        case trace::views::AggOp::Active:
            return false;
    }
    return false;
}

// Agg ops + reduced fields the tier can serve, shared by the EVENT-only and the
// unified events+profiles paths (which differ only on group-key allowances).
bool aggs_and_fields_answerable(const trace::views::detail::ViewPlan& plan,
                                const trace::views::detail::AggSchema& sch) {
    for (const auto& s : plan.agg) {
        if (!tier_serves_op(s.op)) return false;
        // Only dur/size persist m3/m4 and a DDSketch; skew/kurtosis,
        // percentiles and histograms on any other field must scan.
        if ((s.op == trace::views::AggOp::Skew ||
             s.op == trace::views::AggOp::Kurt ||
             s.op == trace::views::AggOp::Pct ||
             s.op == trace::views::AggOp::Hist) &&
            s.field != "dur" && s.field != "size")
            return false;
        if (s.op == trace::views::AggOp::Count && !s.field.empty())
            return false;
        if (s.field == "ts" && s.op != trace::views::AggOp::Min) return false;
        if (s.field == "te" && s.op != trace::views::AggOp::Max) return false;
        // The scan scales ts/dur/te by time_scale; the tier stores raw metrics,
        // so a scaled-field agg would diverge. Decline it.
        if (plan.time_scale != 1.0 &&
            (s.field == "ts" || s.field == "dur" || s.field == "te" ||
             s.by == "ts" || s.by == "dur" || s.by == "te"))
            return false;
    }
    for (const auto& f : sch.fields)
        if (!answerable_field(f)) return false;
    return true;
}

bool answerable(const trace::views::detail::ViewPlan& plan,
                const trace::views::detail::AggSchema& sch) {
    if (plan.time_bucket_us != 0 || plan.time_range || plan.files.empty() ||
        !plan.computed.empty())
        return false;
    if (plan.query && !query_answerable(*plan.query)) return false;
    for (const auto& gk : plan.group_by) {
        if (gk.kind == trace::views::GroupKey::Kind::Arg ||
            gk.kind == trace::views::GroupKey::Kind::Field ||
            gk.kind == trace::views::GroupKey::Kind::Rank)
            return false;
        if (gk.kind == trace::views::GroupKey::Kind::Resolved) {
            const auto key = trace::views::detail::resolved_key_field(gk);
            if (key != "fhash" && key != "hhash") return false;
        }
    }
    return aggs_and_fields_answerable(plan, sch);
}

// Group value from a CF key, byte-identical to view_aggregate group_value().
std::string key_value(const AggKeyView& kv, const trace::views::GroupKey& gk,
                      char (&fbuf)[::dftracer::utils::hash::HEX64_DIGITS]) {
    switch (gk.kind) {
        case trace::views::GroupKey::Kind::Name:
            return std::string(kv.name);
        case trace::views::GroupKey::Kind::Cat:
            return std::string(kv.cat);
        case trace::views::GroupKey::Kind::Pid:
            return std::to_string(kv.pid);
        case trace::views::GroupKey::Kind::Tid:
            return std::to_string(kv.tid);
        case trace::views::GroupKey::Kind::Fhash:
        case trace::views::GroupKey::Kind::FilePath:
        case trace::views::GroupKey::Kind::FileName:
            return std::string(agg::fhash_text(kv, fbuf));
        case trace::views::GroupKey::Kind::Hhash:
        case trace::views::GroupKey::Kind::HostName:
            return std::string(kv.hhash);
        case trace::views::GroupKey::Kind::IoCat:
            return std::to_string(
                static_cast<int>(trace::internal::io_category(kv.name)));
        case trace::views::GroupKey::Kind::AccPat:
            return "0";
        case trace::views::GroupKey::Kind::Rank:
            // Never reached: Rank keeps a plan off the tier (answerable()).
            return std::to_string(kv.pid);
        case trace::views::GroupKey::Kind::Expr:
            // Never reached: a computed column keeps a plan off the tier.
            return {};
        case trace::views::GroupKey::Kind::Resolved:
            // Only fhash and hhash keys reach the tier (answerable()).
            if (trace::views::detail::resolved_key_field(gk) == "fhash")
                return std::string(agg::fhash_text(kv, fbuf));
            return std::string(kv.hhash);
        case trace::views::GroupKey::Kind::Arg:
        case trace::views::GroupKey::Kind::Field:
            // Resolved from the key's extra_keys (e.g. PROFILE epoch/step);
            // requires the key to have been parsed with want_extra_keys.
            for (const auto& [k, v] : kv.extra_keys)
                if (k == gk.arg) return std::string(v);
            return {};
    }
    return {};
}

void fill_field(dataframe::FieldStat& fs, const std::string& field,
                const AggMetricsFullView& mv) {
    // Tier metrics are uint64; restore the exact integer domain (sum/min/max)
    // so a tier-answered Min/Max/Sum matches the scan path past 2^53.
    auto set_exact = [&](std::uint64_t total, std::uint64_t mn,
                         std::uint64_t mx) {
        fs.domain = dftracer::utils::dataframe::FieldStatDomain::U64;
        fs.esum = std::bit_cast<std::int64_t>(total);
        fs.emin = std::bit_cast<std::int64_t>(mn);
        fs.emax = std::bit_cast<std::int64_t>(mx);
    };
    if (field == "dur") {
        fs.n = mv.count;
        fs.sum = static_cast<double>(mv.dur_total);
        fs.sumsq = mv.dur_m2;
        fs.m3 = mv.dur_m3;
        fs.m4 = mv.dur_m4;
        fs.min = static_cast<double>(mv.dur_min);
        fs.max = static_cast<double>(mv.dur_max);
        set_exact(mv.dur_total, mv.dur_min, mv.dur_max);
    } else if (field == "size") {
        if (mv.size_total == 0) return;  // no size events: leave absent
        fs.n = mv.count;
        fs.sum = static_cast<double>(mv.size_total);
        fs.sumsq = mv.size_m2;
        fs.m3 = mv.size_m3;
        fs.m4 = mv.size_m4;
        fs.min = static_cast<double>(mv.size_min);
        fs.max = static_cast<double>(mv.size_max);
        set_exact(mv.size_total, mv.size_min, mv.size_max);
    } else if (field == "offset") {
        fs.n = mv.count;
        fs.sum = static_cast<double>(mv.offset_total);
        fs.sumsq = mv.offset_m2;
        fs.m3 = mv.offset_m3;
        fs.m4 = mv.offset_m4;
        fs.min = static_cast<double>(mv.offset_min);
        fs.max = static_cast<double>(mv.offset_max);
        set_exact(mv.offset_total, mv.offset_min, mv.offset_max);
    } else if (field == "ts") {
        fs.n = mv.count;
        fs.min = fs.max = static_cast<double>(mv.ts);
        set_exact(mv.ts, mv.ts, mv.ts);
    } else if (field == "te") {
        fs.n = mv.count;
        fs.min = fs.max = static_cast<double>(mv.te);
        set_exact(mv.te, mv.te, mv.te);
    }
}

// Per-index tier state, resolved once and reused across collects. The read-only
// aggregation DB is opened a single time (not per collect), the intern loaded
// once, and the index's file set cached for the coverage check. Immutable once
// built: a rebuilt index (detected by `mtime`) swaps in a new entry, so a
// reader holding the shared_ptr keeps a consistent snapshot.
struct TierCache {
    bool has_tier = false;
    bool has_sketches = false;  // CF stored DDSketches (compute_percentiles)
    bool group_by_file = true;  // CF keys carry fhash
    StringViewSet files;
    std::shared_ptr<agg::AggDbHandle> handle;
    std::int64_t mtime = 0;     // RocksDB CURRENT mtime; 0 = absent
    std::uint64_t interval_us =
        0;                      // CF aggregation interval (stored bucket size)
};

// Mtime of the aggregation DB's CURRENT file, rewritten on every manifest roll,
// so it changes when the index is rebuilt. A cheap stat, not a content read.
std::int64_t current_mtime(const std::string& index_path) {
    std::error_code ec;
    auto t = fs::last_write_time(fs::path(index_path) / "CURRENT", ec);
    if (ec) return 0;
    return static_cast<std::int64_t>(t.time_since_epoch().count());
}

std::shared_ptr<const TierCache> build_tier_cache(const std::string& index_path,
                                                  std::int64_t mtime) {
    auto tc = std::make_shared<TierCache>();
    tc->mtime = mtime;
    try {
        std::string err;
        auto handle = agg::open_agg_db(index_path, err);
        if (handle) {
            tc->group_by_file = handle->config.group_by_file;
            tc->interval_us = handle->config.time_interval_us;
            // Only files whose rows the tier holds at its current params.
            IndexDatabase db(index_path, index::store::IndexOpenMode::ReadOnly);
            for (auto& [p, id] : db.query_all_file_info_ids()) {
                auto state =
                    db.extension_state(id, index::store::IndexExtension::AGG);
                if (state && state->ready &&
                    state->version == index::store::layout::ext_version(
                                          index::store::IndexExtension::AGG) &&
                    state->params_hash == handle->config.params_hash)
                    tc->files.insert(p);
            }
            tc->has_tier = true;
            tc->handle = std::shared_ptr<agg::AggDbHandle>(std::move(handle));
            // Peek one EVENT row to learn whether the tier stored sketches
            // (compute_percentiles); percentile queries need them.
            agg::tier::for_each_row(
                *tc->handle->db, 0, agg::AGG_KEY_NUM_SHARDS,
                [&](std::string_view kd, std::string_view vd) {
                    AggKeyView kvp;
                    if (!agg::parse_agg_key_view(kd, tc->handle->agg->intern(),
                                                 kvp) ||
                        kvp.map_type != agg::AggMapType::EVENT)
                        return true;
                    AggMetricsFullView mvp;
                    if (agg::parse_agg_value_full_view(vd, mvp))
                        tc->has_sketches =
                            !mvp.dur_sketch.empty() || !mvp.size_sketch.empty();
                    return false;
                });
        }
    } catch (const std::exception&) {
        auto empty = std::make_shared<TierCache>();
        empty->mtime = mtime;  // incomplete/unreadable index: fall back to scan
        return empty;
    }
    return tc;
}

std::shared_mutex g_tier_mtx;

// Never destructed: entries hold open RocksDB handles, and running this map's
// destructor at process exit races RocksDB's own static teardown (SyncPoint)
// -> heap-use-after-free. clear_tier_cache() (a rocksdb pre-exit hook) empties
// it cleanly during orderly shutdown instead.
StringViewMap<std::shared_ptr<const TierCache>>& tier_cache_map() {
    static dftracer::utils::NoDestructor<
        StringViewMap<std::shared_ptr<const TierCache>>>
        m;
    return *m;
}

// The cache holds each index's read-only agg DB open for reuse. It is dropped
// from a rocksdb pre-exit cleanup (see register_pre_exit_cleanup) so the DBs
// close cleanly before RocksDB starts abandoning handles at process exit; a
// plain std::atexit would run too late and leak them via that abandon path.
void clear_tier_cache() {
    std::unique_lock<std::shared_mutex> lk(g_tier_mtx);
    tier_cache_map().clear();
}

void evict_tier_cache(const std::string& index_path) {
    std::unique_lock<std::shared_mutex> lk(g_tier_mtx);
    tier_cache_map().erase(index_path);
}

std::shared_ptr<const TierCache> tier_cache(const std::string& index_path) {
    static const int registered = [] {
        index::store::register_pre_exit_cleanup(clear_tier_cache);
        // Drop this index's cached agg DB when its manager entry is reset (e.g.
        // a rebuild removing the directory), so the tier does not keep the DB
        // open and block the removal.
        index::store::register_reset_listener(evict_tier_cache);
        return 0;
    }();
    (void)registered;
    const std::int64_t mtime = current_mtime(index_path);
    {
        std::shared_lock<std::shared_mutex> rlk(g_tier_mtx);
        if (auto it = tier_cache_map().find(index_path);
            it != tier_cache_map().end() && it->second->mtime == mtime)
            return it->second;
    }
    // Build outside the lock: build_tier_cache -> open_agg_db may reset() the
    // index (its merge-operator open falling back), which fires the tier's own
    // reset listener and re-locks g_tier_mtx. Holding it here would self-lock.
    // The build is idempotent, so a concurrent double-build just races to
    // insert; last write wins and both hold the same data for this mtime.
    auto tc = build_tier_cache(index_path, mtime);
    std::unique_lock<std::shared_mutex> wlk(g_tier_mtx);
    if (auto it = tier_cache_map().find(index_path);
        it != tier_cache_map().end() && it->second->mtime == mtime)
        return it->second;
    tier_cache_map()[index_path] =
        tc;  // replaces stale; old readers keep theirs
    return tc;
}

// Whether the plan needs per-file resolution, which a tier built with
// group_by_file=false folded away and cannot reconstruct.
bool plan_needs_file(const trace::views::detail::ViewPlan& plan) {
    for (const auto& g : plan.group_by)
        if (g.kind == trace::views::GroupKey::Kind::Fhash ||
            g.kind == trace::views::GroupKey::Kind::FilePath ||
            g.kind == trace::views::GroupKey::Kind::FileName ||
            (g.kind == trace::views::GroupKey::Kind::Resolved &&
             trace::views::detail::resolved_key_field(g) == "fhash"))
            return true;
    if (plan.query)
        for (std::string_view f : plan.query->fields())
            if (f == "fhash") return true;
    return false;
}

// The tier cache iff it exists and spans exactly the plan's files (the CF is
// keyed by content fhash, so a subset view cannot filter it exactly). Else
// null.
std::shared_ptr<const TierCache> covering_tier(
    const trace::views::detail::ViewPlan& plan) {
    if (plan.files.empty()) return nullptr;
    const std::string& index_path = plan.files.front().index_path;
    if (index_path.empty()) return nullptr;
    auto tc = tier_cache(index_path);
    if (!tc->has_tier) return nullptr;
    if (!tc->group_by_file && plan_needs_file(plan)) return nullptr;
    if (tc->files.size() != plan.files.size()) return nullptr;
    for (const auto& f : plan.files) {
        if (f.index_path != index_path) return nullptr;
        if (!tc->files.count(get_logical_path(f.file_path))) return nullptr;
    }
    return tc;
}

}  // namespace

namespace {

// A value column the tier can fill from a stored MetricStats row: its AggState
// value_col index and the base field (dur/size/offset/ts/te) whose stat feeds
// it. Derived once per collect from the engine's lowered value column names.
struct TierValueCol {
    std::int32_t value_col;
    std::string field;
};

std::vector<TierValueCol> tier_value_cols(
    const std::vector<std::string>& value_names) {
    std::vector<TierValueCol> out;
    for (std::size_t vc = 0; vc < value_names.size(); ++vc) {
        std::string field =
            trace::views::detail::agg_value_base_field(value_names[vc]);
        if (!answerable_field(field)) continue;  // e.g. a SetUnion text column
        out.push_back({static_cast<std::int32_t>(vc), std::move(field)});
    }
    return out;
}

// One CF row's reduced stats as engine seed values, one per fillable value
// column (an absent field stays a default n==0 FieldStat so the finalized
// column domain matches the scan). A dur/size column carries its stored
// DDSketch.
std::vector<dataframe::AggSeedValue> row_seed_values(
    const std::vector<TierValueCol>& cols, const AggMetricsFullView& mv,
    std::vector<utilities::common::statistics::DDSketch>& sketch_store) {
    std::vector<dataframe::AggSeedValue> out;
    out.reserve(cols.size());
    sketch_store.clear();
    sketch_store.resize(cols.size());
    for (std::size_t i = 0; i < cols.size(); ++i) {
        dataframe::AggSeedValue v;
        v.value_col = cols[i].value_col;
        fill_field(v.stat, cols[i].field, mv);
        const std::string_view blob = cols[i].field == "dur" ? mv.dur_sketch
                                      : cols[i].field == "size"
                                          ? mv.size_sketch
                                          : std::string_view();
        if (!blob.empty()) {
            sketch_store[i] =
                utilities::common::statistics::DDSketch::deserialize(
                    reinterpret_cast<const std::uint8_t*>(blob.data()),
                    blob.size());
            v.sketch = &sketch_store[i];
        }
        out.push_back(std::move(v));
    }
    return out;
}

// Whether a group key relabels an opaque hash to a name post-aggregation
// (finalize_engine_result resolves these), matching key_is_resolved in the
// engine tail.
bool tier_key_is_resolved(trace::views::GroupKey::Kind kind) {
    return kind == trace::views::GroupKey::Kind::FilePath ||
           kind == trace::views::GroupKey::Kind::FileName ||
           kind == trace::views::GroupKey::Kind::HostName ||
           kind == trace::views::GroupKey::Kind::Rank ||
           kind == trace::views::GroupKey::Kind::Resolved;
}

void build_tier_keys(std::vector<std::string>& keys,
                     const trace::views::detail::ViewPlan& plan,
                     const AggKeyView& kv,
                     const index::plan::GroupResolver* resolver,
                     char (&fbuf)[::dftracer::utils::hash::HEX64_DIGITS]) {
    for (const auto& gk : plan.group_by) {
        std::string v = key_value(kv, gk, fbuf);
        if (gk.transform != trace::views::GroupKey::Transform::None) {
            if (resolver && tier_key_is_resolved(gk.kind))
                v = trace::views::detail::resolve_group_value(*resolver, gk, v);
            v = trace::views::detail::apply_group_transform(gk, std::move(v));
        }
        keys.push_back(std::move(v));
    }
}

}  // namespace

// `eval_root` is the cat-lowered clone of plan.query's AST (see
// clone_cat_lowered), built once by the caller; the tier's cat values are
// lowercased, so cat literals must be too. Non-null whenever plan.query is set.
static bool key_passes_query(
    const trace::views::detail::ViewPlan& plan,
    const query::QueryNode* eval_root, const AggKeyView& kv,
    char (&fbuf)[::dftracer::utils::hash::HEX64_DIGITS]) {
    if (!plan.query) return true;
    query::ValueMap vm;
    for (std::string_view f : plan.query->fields()) {
        if (f == "name")
            vm[f] = std::string(kv.name);
        else if (f == "cat")
            vm[f] = std::string(kv.cat);
        else if (f == "pid")
            vm[f] = kv.pid;
        else if (f == "tid")
            vm[f] = kv.tid;
        else if (f == "hhash")
            vm[f] = std::string(kv.hhash);
        else if (f == "fhash")
            vm[f] = std::string(agg::fhash_text(kv, fbuf));
    }
    return query::evaluate(*eval_root, vm);
}

bool agg_tier_collect(const trace::views::detail::ViewPlan& plan,
                      dataframe::AggStatePtr& out) {
    const trace::views::detail::AggSchema& sch =
        trace::views::detail::ensure_schema(plan);
    if (!answerable(plan, sch)) return false;

    const auto tc = covering_tier(plan);
    if (!tc) return false;

    // Percentiles and histograms need the stored DDSketch; if the tier has
    // none, scan instead.
    if (!tc->has_sketches)
        for (const auto& s : plan.agg)
            if (s.op == trace::views::AggOp::Pct ||
                s.op == trace::views::AggOp::Hist)
                return false;

    // Build the same engine AggState the scan would, so finalize_engine_result
    // renders it byte-for-byte identically; seed each group from the stored
    // per-key MetricStats instead of scanning events.
    trace::views::detail::AggInputSpec spec = make_agg_input_spec(plan);
    dataframe::LoweredGroupAggs lowered =
        dataframe::lower_group_aggs(spec.gaggs);
    auto state = dataframe::agg_new(lowered.specs, spec.dyn_specs);
    dataframe::agg_seed_begin(*state, plan.group_by.size());
    const std::vector<TierValueCol> vcols =
        tier_value_cols(lowered.value_names);

    // A transformed key needs the resolved+transformed value at seed time
    // (finalize_engine_result skips resolver for transformed keys); a plain
    // resolved key is seeded as its raw hash and resolved at finalize.
    bool any_transform = false;
    for (const auto& gk : plan.group_by)
        if (gk.transform != trace::views::GroupKey::Transform::None)
            any_transform = true;
    const index::plan::GroupResolver* resolver =
        any_transform ? ensure_resolver(plan) : nullptr;

    const StringIntern& intern = tc->handle->agg->intern();
    char fbuf[::dftracer::utils::hash::HEX64_DIGITS];
    std::vector<std::string> keys;
    std::vector<utilities::common::statistics::DDSketch> sketch_store;
    const auto eval_root =
        plan.query ? clone_cat_lowered(plan.query->root()) : nullptr;
    agg::tier::for_each_row(
        *tc->handle->db, 0, agg::AGG_KEY_NUM_SHARDS,
        [&](std::string_view kdata, std::string_view vdata) {
            AggKeyView kv;
            if (!agg::parse_agg_key_view(kdata, intern, kv)) return true;
            if (kv.map_type != agg::AggMapType::EVENT) return true;
            if (!key_passes_query(plan, eval_root.get(), kv, fbuf)) return true;
            AggMetricsFullView mv;
            if (!agg::parse_agg_value_full_view(vdata, mv)) return true;
            keys.clear();
            keys.reserve(plan.group_by.size());
            build_tier_keys(keys, plan, kv, resolver, fbuf);
            dataframe::agg_seed_group(*state, keys, mv.count,
                                      row_seed_values(vcols, mv, sketch_store));
            return true;
        });
    dataframe::agg_seed_finalize(*state);
    out = std::move(state);
    return true;
}

// The bucket value for a stored key, matching view_aggregate's scan-path
// formula floor(ts*time_scale/interval)*interval. kv.time_bucket is already the
// stored bucket-start timestamp (compute_time_bucket floors ts to the CF
// interval), so it plays the role of ts here.
static std::int64_t tier_time_bucket(
    const AggKeyView& kv, const trace::views::detail::ViewPlan& plan) {
    const double start_us = static_cast<double>(kv.time_bucket);
    const double interval = static_cast<double>(plan.time_bucket_us);
    return static_cast<std::int64_t>(start_us * plan.time_scale / interval) *
           static_cast<std::int64_t>(plan.time_bucket_us);
}

bool events_profiles_collect(const trace::views::detail::ViewPlan& plan,
                             dataframe::DataFrame& out_regular,
                             dataframe::DataFrame& out_aggregated,
                             int shard_begin, int shard_end,
                             const trace::views::ProgressFn* progress) {
    const trace::views::detail::AggSchema& sch =
        trace::views::detail::ensure_schema(plan);
    // time_range() is a window filter the tier cannot evaluate; time_bucket()
    // maps onto the CF's stored bucket grain and is supported.
    if (plan.time_range) return false;
    if (plan.query && !query_answerable(*plan.query)) return false;
    if (!aggs_and_fields_answerable(plan, sch)) return false;

    const auto tc = covering_tier(plan);
    if (!tc) return false;
    if (!tc->has_sketches)
        for (const auto& s : plan.agg)
            if (s.op == trace::views::AggOp::Pct ||
                s.op == trace::views::AggOp::Hist)
                return false;

    const bool has_bucket = plan.time_bucket_us > 0;
    // The tier can only coarsen the stored grain: a requested bucket finer than
    // (or not a multiple of) the CF interval cannot be produced exactly.
    if (has_bucket && tc->interval_us > 0 &&
        (plan.time_bucket_us < tc->interval_us ||
         plan.time_bucket_us % tc->interval_us != 0))
        return false;
    // Any Arg group key (e.g. epoch/step) lives in the PROFILE key's
    // extra_keys.
    bool want_extra = false;
    for (const auto& gk : plan.group_by)
        if (gk.kind == trace::views::GroupKey::Kind::Arg) want_extra = true;
    // A transformed key needs its resolved+transformed value at seed time
    // (finalize resolves the plain resolved keys itself).
    bool any_transform = false;
    for (const auto& gk : plan.group_by)
        if (gk.transform != trace::views::GroupKey::Transform::None)
            any_transform = true;
    const index::plan::GroupResolver* resolver =
        any_transform ? ensure_resolver(plan) : nullptr;

    // Two engine AggStates (events, profiles); each keyed [time_bucket?,
    // group_by...] and finalized like a fresh scan. Seed from the stored
    // MetricStats rows, split by map type.
    const std::size_t nkeys = (has_bucket ? 1u : 0u) + plan.group_by.size();
    trace::views::detail::AggInputSpec ispec = make_agg_input_spec(plan);
    dataframe::LoweredGroupAggs lowered =
        dataframe::lower_group_aggs(ispec.gaggs);
    const std::vector<TierValueCol> vcols =
        tier_value_cols(lowered.value_names);
    auto ev_state = dataframe::agg_new(lowered.specs, ispec.dyn_specs);
    auto prof_state = dataframe::agg_new(lowered.specs, ispec.dyn_specs);
    dataframe::agg_seed_begin(*ev_state, nkeys);
    dataframe::agg_seed_begin(*prof_state, nkeys);

    const StringIntern& intern = tc->handle->agg->intern();
    char fbuf[::dftracer::utils::hash::HEX64_DIGITS];
    std::vector<std::string> keys;
    std::vector<utilities::common::statistics::DDSketch> sketch_store;
    if (shard_end <= 0) shard_end = agg::AGG_KEY_NUM_SHARDS;
    const std::size_t total_shards =
        static_cast<std::size_t>(shard_end - shard_begin);
    int reported_shard = shard_begin;
    if (progress && *progress) (*progress)(0, total_shards);
    // Keys begin with a big-endian 2-byte shard prefix, so a shard range is a
    // contiguous key range and no group straddles shards.
    const auto eval_root =
        plan.query ? clone_cat_lowered(plan.query->root()) : nullptr;
    agg::tier::for_each_row(
        *tc->handle->db, static_cast<std::uint16_t>(shard_begin),
        static_cast<std::uint16_t>(shard_end),
        [&](std::string_view kdata, std::string_view vdata) {
            const int shard = (static_cast<unsigned char>(kdata[0]) << 8) |
                              static_cast<unsigned char>(kdata[1]);
            if (progress && *progress && shard > reported_shard) {
                reported_shard = shard;
                (*progress)(static_cast<std::size_t>(shard - shard_begin),
                            total_shards);
            }
            AggKeyView kv;
            if (!agg::parse_agg_key_view(kdata, intern, kv, want_extra))
                return true;
            const bool is_profile = kv.map_type == agg::AggMapType::PROFILE;
            if (kv.map_type != agg::AggMapType::EVENT && !is_profile)
                return true;
            if (!key_passes_query(plan, eval_root.get(), kv, fbuf)) return true;

            AggMetricsFullView mv;
            if (!agg::parse_agg_value_full_view(vdata, mv)) return true;

            keys.clear();
            keys.reserve(nkeys);
            // Key layout [time_bucket?, group_by...]: a plain resolved key is
            // seeded as its raw hash (finalize resolves it); a transformed key
            // is resolved+transformed here since finalize skips the resolver
            // for it.
            if (has_bucket)
                keys.push_back(std::to_string(tier_time_bucket(kv, plan)));
            build_tier_keys(keys, plan, kv, resolver, fbuf);
            dataframe::agg_seed_group(is_profile ? *prof_state : *ev_state,
                                      keys, mv.count,
                                      row_seed_values(vcols, mv, sketch_store));
            return true;
        });

    dataframe::agg_seed_finalize(*ev_state);
    dataframe::agg_seed_finalize(*prof_state);
    out_regular = trace::views::detail::finalize_engine_result(*ev_state, plan);
    out_aggregated =
        trace::views::detail::finalize_engine_result(*prof_state, plan);
    if (progress && *progress) (*progress)(total_shards, total_shards);
    return true;
}

bool system_collect(const trace::views::detail::ViewPlan& plan,
                    dataframe::DataFrame& out_table, int shard_begin,
                    int shard_end, const trace::views::ProgressFn*) {
    // The plan's event-side time_bucket/group_by do not apply here - system
    // rows carry their own (host,name,time_bucket) grain. The CF now has the
    // same shard prefix as the AGG CF, so it range-splits identically.
    const auto tc = covering_tier(plan);
    if (!tc) return false;
    if (shard_end <= 0) shard_end = agg::AGG_KEY_NUM_SHARDS;

    auto for_each =
        [&](const std::function<void(std::string_view, std::string_view)>& cb) {
            agg::tier::for_each_system_row(
                *tc->handle->db, static_cast<std::uint16_t>(shard_begin),
                static_cast<std::uint16_t>(shard_end),
                [&cb](std::string_view k, std::string_view v) {
                    cb(k, v);
                    return true;
                });
        };

    // First pass: the sorted union of metric names, so the schema is stable.
    std::set<std::string> name_set;
    for_each([&](std::string_view, std::string_view val) {
        auto m = agg::deserialize_system_value(val);
        if (m.metrics)
            for (const auto& [n, _] : *m.metrics) name_set.insert(n);
    });
    const std::vector<std::string> metric_names(name_set.begin(),
                                                name_set.end());

    // System rows carry their own grain and a per-metric mean (a double), so
    // the value columns stay Float64 - no integer domain to preserve here.
    std::vector<std::string> hosts, names;
    std::vector<double> time_bucket, ts, te, count;
    std::vector<std::vector<double>> metric_vals(metric_names.size());
    for_each([&](std::string_view key, std::string_view val) {
        auto k = agg::deserialize_system_key(key);
        auto m = agg::deserialize_system_value(val);
        hosts.emplace_back(k.key.hhash);
        names.emplace_back(k.key.name);
        time_bucket.push_back(static_cast<double>(k.key.time_bucket));
        ts.push_back(static_cast<double>(m.ts));
        te.push_back(static_cast<double>(m.te));
        count.push_back(static_cast<double>(m.count));
        for (std::size_t i = 0; i < metric_names.size(); ++i) {
            double v = std::numeric_limits<double>::quiet_NaN();
            if (m.metrics) {
                auto it2 = m.metrics->find(metric_names[i]);
                if (it2 != m.metrics->end()) v = it2->second.mean();
            }
            metric_vals[i].push_back(v);
        }
    });

    dataframe::DataFrame b;
    auto add_str = [&](const char* nm, const std::vector<std::string>& v) {
        b.names.emplace_back(nm);
        b.columns.push_back(dataframe::Series::strings(v));
    };
    auto add_f64 = [&](const std::string& nm, const std::vector<double>& v) {
        b.names.push_back(nm);
        b.columns.push_back(dataframe::Series::flat_f64(
            v.data(), static_cast<std::int64_t>(v.size())));
    };
    add_str("host_hash", hosts);
    add_str("name", names);
    add_f64("time_bucket", time_bucket);
    add_f64("ts", ts);
    add_f64("te", te);
    add_f64("count", count);
    for (std::size_t i = 0; i < metric_names.size(); ++i)
        add_f64(metric_names[i], metric_vals[i]);
    out_table = std::move(b);
    return true;
}

}  // namespace dftracer::utils::index::schemas::dft::agg

#else  // !DFTRACER_UTILS_ENABLE_ARROW

namespace dftracer::utils::index::schemas::dft::agg {
bool agg_tier_collect(const trace::views::detail::ViewPlan&,
                      dftracer::utils::dataframe::AggStatePtr&) {
    return false;
}
bool events_profiles_collect(const trace::views::detail::ViewPlan&,
                             dftracer::utils::dataframe::DataFrame&,
                             dftracer::utils::dataframe::DataFrame&, int, int,
                             const trace::views::ProgressFn*) {
    return false;
}
bool system_collect(const trace::views::detail::ViewPlan&,
                    dftracer::utils::dataframe::DataFrame&, int, int,
                    const trace::views::ProgressFn*) {
    return false;
}
}  // namespace dftracer::utils::index::schemas::dft::agg

#endif
