#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/trace/views/aggfold.h>
#include <dftracer/utils/trace/views/batch_bridge.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/rollup_store.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/trace/views/view_source.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::trace::views::detail {

namespace dataframe = dftracer::utils::dataframe;

// Hidden columns the shared derivation materializes on top of the base frame.
// Both the streaming path (prepare_engine_group's with_column exprs) and the
// fused fold (build_agg_input_frame) key/scale on these exact names.
static constexpr const char* SCALED_TS_COL = "__view_agg_engine_scaled_ts";
static constexpr const char* SCALED_DUR_COL = "__view_agg_engine_scaled_dur";
static constexpr const char* SCALED_TE_COL = "__view_agg_engine_scaled_te";
static constexpr const char* CAT_KEY_COL = "__view_agg_engine_cat_key";
static constexpr const char* BUCKET_KEY_COL = "__view_agg_engine_time_bucket";

namespace {

// The frame column name the raw scan produces for `field` fed as a value/by
// column: a top-level field keeps its name, an arg field becomes args.<name>
// (canonical_row_column_name), so the group_by references the column the scan
// emits. Empty field stays empty (Count()'s neutral value column).
std::string value_col_name(const std::string& field) {
    return field.empty() ? std::string() : canonical_row_column_name(field);
}

// A field agg_field_typed_t/agg_field_t derives instead of reading straight
// (size = io-cat byte size, te = ts+dur). The raw scan builds it as a typed
// derived column (AGG_DERIVED_PREFIX / derived_agg_column) rather than reading
// a stored field.
bool is_derived_field(const std::string& f) { return f == "size" || f == "te"; }

// The frame column name a value/by field's group agg reads: a derived field
// (size/te) keeps its own name (build_row_frame emits it under that name from
// the derived token), any other field maps through value_col_name.
std::string value_col(const std::string& field) {
    return is_derived_field(field) ? field : value_col_name(field);
}

// The raw-scan select token that produces a value/by field's column: a derived
// field routes through AGG_DERIVED_PREFIX (a typed derived column), any other
// field is selected by its own name. Empty field stays empty.
std::string value_select_token(const std::string& field) {
    if (field.empty()) return std::string();
    return is_derived_field(field) ? std::string(AGG_DERIVED_PREFIX) + field
                                   : field;
}

// ts/dur/te accumulate in the unrounded time_scale domain (agg_fold.h's
// field_scaled); a value agg over one needs the engine-side rescale path.
bool is_scaled_field(const std::string& f) {
    return f == "ts" || f == "dur" || f == "te";
}

dataframe::Agg to_engine_agg(AggOp op) {
    switch (op) {
        case AggOp::Count:
            return dataframe::Agg::Count;
        case AggOp::Sum:
            return dataframe::Agg::Sum;
        case AggOp::Min:
            return dataframe::Agg::Min;
        case AggOp::Max:
            return dataframe::Agg::Max;
        case AggOp::Mean:
            return dataframe::Agg::Mean;
        case AggOp::Var:
            return dataframe::Agg::Var;
        case AggOp::Std:
            return dataframe::Agg::Std;
        case AggOp::Skew:
            return dataframe::Agg::Skew;
        case AggOp::Kurt:
            return dataframe::Agg::Kurt;
        case AggOp::SumSq:
            return dataframe::Agg::SumSq;
        case AggOp::Pct:
            return dataframe::Agg::Pct;
        case AggOp::Hist:
            return dataframe::Agg::Hist;
        case AggOp::ArgMax:
            return dataframe::Agg::ArgMax;
        case AggOp::SetUnion:
            return dataframe::Agg::SetUnion;
        case AggOp::Busy:
            return dataframe::Agg::Busy;
        case AggOp::Concurrency:
            return dataframe::Agg::Concurrency;
        case AggOp::Utilization:
            return dataframe::Agg::Utilization;
        case AggOp::Active:
            return dataframe::Agg::Active;
    }
    throw DFTUtilsException::cat(ErrorCode::INTERNAL,
                                 "agg engine: unknown aggregate op");
}

dataframe::GroupAgg to_group_agg(const AggSpec& spec) {
    dataframe::GroupAgg g;
    g.op = to_engine_agg(spec.op);
    g.out = agg_col_name(spec);
    g.param = spec.q;
    if (spec.op == AggOp::ArgMax) {
        g.column = value_col(spec.field);
        g.by = value_col(spec.by);
    } else if (is_occupancy_op(spec.op)) {
        // Occupancy has no value field; it reads the raw (ts, dur) pair, with
        // the endpoint-snap tolerance carried in param.
        g.column = "ts";
        g.by = "dur";
    } else if (spec.op == AggOp::Count) {
        // Count() is the group row count; Count(field) counts only the
        // field-present rows, which the engine's CountValid reads from the
        // field's per-group stat (FieldStat::n).
        if (!spec.field.empty()) {
            g.op = dataframe::Agg::CountValid;
            g.column = value_col(spec.field);
        }
    } else {
        g.column = value_col(spec.field);
    }
    return g;
}

// The final group-key column must be a String (group keys are text), but
// agg_finalize keeps a non-string key's native type (Int64/Uint64/Float64).
// Render it back to the decimal text form.
dataframe::Series key_column_to_string(const dataframe::Series& col) {
    using dataframe::TypeId;
    const std::int64_t n = col.length();
    std::vector<std::string> vals(static_cast<std::size_t>(n));
    switch (col.type()) {
        case TypeId::String:
            return col.share();
        case TypeId::Int64: {
            const std::int64_t* d = col.data<std::int64_t>();
            for (std::int64_t i = 0; i < n; ++i)
                vals[static_cast<std::size_t>(i)] = std::to_string(d[i]);
            break;
        }
        case TypeId::Uint64: {
            const std::uint64_t* d = col.data<std::uint64_t>();
            for (std::int64_t i = 0; i < n; ++i)
                vals[static_cast<std::size_t>(i)] = std::to_string(d[i]);
            break;
        }
        case TypeId::Float64: {
            const double* d = col.data<double>();
            for (std::int64_t i = 0; i < n; ++i)
                vals[static_cast<std::size_t>(i)] = std::to_string(d[i]);
            break;
        }
        default:
            throw DFTUtilsException::cat(
                ErrorCode::INTERNAL,
                "agg engine: unexpected group-key column type");
    }
    return dataframe::Series::strings(vals);
}

// Keys whose group column is an opaque identifier the post-aggregation re-key
// pass relabels to a human-readable name: fhash/hhash for resolved-name keys,
// pid for Rank (the PR-metadata rank map keys on pid).
bool key_is_resolved(GroupKey::Kind kind) {
    return kind == GroupKey::Kind::FilePath ||
           kind == GroupKey::Kind::FileName ||
           kind == GroupKey::Kind::HostName || kind == GroupKey::Kind::Rank;
}

// The raw scan/group-by field a key groups on: fhash/hhash for a resolved
// name key (the fold groups on the hash, a bijection, and relabels to the
// resolved name only after aggregation), pid for Rank (the rank map keys on
// pid, matching agg_fold.h's append_group_dim), a group-key-string sentinel for
// an Arg/Field key (rendered like the engine agg path, then relabeled to
// group_col_name after aggregation), else the key's own column.
std::string key_group_field(const GroupKey& gk) {
    switch (gk.kind) {
        case GroupKey::Kind::FilePath:
        case GroupKey::Kind::FileName:
            return "fhash";
        case GroupKey::Kind::HostName:
            return "hhash";
        case GroupKey::Kind::Rank:
            return "pid";
        case GroupKey::Kind::Arg:
            return std::string(AGG_KEY_ARG_PREFIX) + gk.arg;
        case GroupKey::Kind::Field:
            return std::string(AGG_KEY_FIELD_PREFIX) + gk.arg;
        default:
            return group_col_name(gk);
    }
}

// Resolve one already-stringified hash group-key column to its resolved name,
// matching resolve_group_keys/resolve_group_value exactly (same GroupResolver,
// same FileName-from-FilePath basename derivation).
dataframe::Series resolve_key_column(const dataframe::Series& hashes,
                                     const GroupResolver& resolver,
                                     GroupKey::Kind kind) {
    const std::int64_t n = hashes.length();
    std::vector<std::string> vals(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        vals[static_cast<std::size_t>(i)] = resolve_group_value(
            resolver, kind, std::string(hashes.string_at(i)));
    return dataframe::Series::strings(vals);
}

// One raw group-key column cell rendered exactly as the engine agg path builds
// its key: a String cell verbatim, an integer cell as decimal, a null cell as
// the empty string (append_arg emits nothing for a missing value).
std::string cell_to_key_string(const dataframe::Series& col, std::int64_t r) {
    using dataframe::TypeId;
    if (col.is_null(r)) return std::string();
    switch (col.type()) {
        case TypeId::String:
            return std::string(col.string_at(r));
        case TypeId::Int64:
            return std::to_string(col.data<std::int64_t>()[r]);
        case TypeId::Uint64:
            return std::to_string(col.data<std::uint64_t>()[r]);
        default:
            throw DFTUtilsException::cat(
                ErrorCode::INTERNAL,
                "agg engine: unexpected transform key-column type");
    }
}

// The pre-transform value of group key `gk` for one raw cell, matching the
// engine agg path (resolve_group_keys: resolve_group_value after the fold's key
// rendering). A resolved-name key resolves its hash (or keeps the raw hash when
// no resolver is loaded); cat is lowercased like agg_fold.h's append_group_dim;
// the rest keep their rendered value.
std::string transform_key_base(const GroupKey& gk, std::string raw,
                               const GroupResolver* resolver) {
    if (key_is_resolved(gk.kind))
        return resolver ? resolve_group_value(*resolver, gk.kind, raw) : raw;
    if (gk.kind == GroupKey::Kind::Cat)
        for (char& ch : raw)
            ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
    return raw;
}

// Harvests only the pid -> rank map from PR metadata records, with no group
// aggregation. The map is byte-identical to the one the engine agg path would
// surface (harvest_pr_rank over the same records).
class RankHarvestFold : public Fold {
   public:
    explicit RankHarvestFold(const dftracer::utils::StringIntern& intern)
        : intern_(&intern) {}
    bool accepts(const ScanShape&) const override { return true; }
    bool needs_args() const override { return true; }  // PR args carry the rank
    std::unique_ptr<Fold> slice() const override {
        return std::make_unique<RankHarvestFold>(*intern_);
    }
    void step(const FoldBatch& batch) override {
        for (const auto& ev : batch.events)
            if (ev.phase == RecordPhase::METADATA)
                harvest_pr_rank(ev, *intern_, ranks_);
    }
    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}
    void merge(Fold& other) override {
        auto& o = static_cast<RankHarvestFold&>(other);
        for (auto& [p, r] : o.ranks_) ranks_.emplace(p, std::move(r));
        o.ranks_.clear();
    }
    coro::CoroTask<bool> finalize(const CoverageSet&) override {
        co_return true;
    }
    std::unordered_map<std::uint64_t, std::string>& ranks() { return ranks_; }

   private:
    const dftracer::utils::StringIntern* intern_;
    std::unordered_map<std::uint64_t, std::string> ranks_;
};

dataframe::AggOp to_dyn_op(AggOp op) {
    switch (op) {
        case AggOp::Count:
            return dataframe::AggOp::Count;
        case AggOp::Sum:
            return dataframe::AggOp::Sum;
        case AggOp::Min:
            return dataframe::AggOp::Min;
        case AggOp::Max:
            return dataframe::AggOp::Max;
        case AggOp::Mean:
            return dataframe::AggOp::Mean;
        case AggOp::Var:
            return dataframe::AggOp::Var;
        case AggOp::Std:
            return dataframe::AggOp::Std;
        case AggOp::SumSq:
            return dataframe::AggOp::SumSq;
        case AggOp::Skew:
            return dataframe::AggOp::Skew;
        case AggOp::Kurt:
            return dataframe::AggOp::Kurt;
        case AggOp::Pct:
            return dataframe::AggOp::Pct;
        case AggOp::ArgMax:
        case AggOp::Hist:
        case AggOp::SetUnion:
        case AggOp::Busy:
        case AggOp::Concurrency:
        case AggOp::Utilization:
        case AggOp::Active:
            break;
    }
    throw DFTUtilsException::cat(ErrorCode::INTERNAL,
                                 "agg engine: op has no per-arg dyn reduction");
}

// The dyn reductions for an auto_numeric_metrics plan, matching the canonical
// dyn columns: an empty numeric_arg_aggs is the legacy bare-named per-arg mean;
// each explicit reduction becomes one AggDynSpec whose out_prefix (dyn_col_name
// with an empty key) gives the finalized column name out_prefix + arg.
std::vector<dataframe::AggDynSpec> build_dyn_specs(const ViewPlan& plan) {
    std::vector<dataframe::AggDynSpec> out;
    if (!plan.auto_numeric_metrics) return out;
    if (plan.numeric_arg_aggs.empty()) {
        out.push_back({dataframe::AggOp::Mean, 0.0, std::string()});
        return out;
    }
    for (const AggSpec& r : plan.numeric_arg_aggs)
        out.push_back({to_dyn_op(r.op), r.q, dyn_col_name(r, std::string())});
    return out;
}

// Rank is a query-time side channel: pid -> rank lives in PR metadata records,
// not the event columns the engine streams. The rank group_by makes make_vdef
// keep the PR metadata; a RankHarvestFold reads the map, fed to the resolver
// the post-aggregation re-key reads.
coro::CoroTask<void> harvest_ranks(const ViewPlan& plan) {
    ViewPlan hp = plan;
    hp.group_by.assign(1, GroupKey::rank());
    hp.agg.clear();
    hp.numeric_arg_aggs.clear();
    hp.auto_numeric_metrics = false;
    hp.time_bucket_us = 0;
    hp.bucket_origin_us = 0;
    hp.bucket_origin_min = false;
    hp.materialize = false;
    hp.sort_col.clear();
    hp.topk_col.clear();
    hp.offset = 0;
    hp.limit = 0;
    hp.select.clear();
    hp.schema.reset();
    ensure_schema(hp);
    ViewDefinition vdef = make_vdef(hp, /*for_aggregation=*/true);
    dftracer::utils::StringIntern intern;
    RankHarvestFold rf(intern);
    std::array<Fold*, 1> folds{&rf};
    co_await fuse(hp, vdef, folds, intern);
    apply_ranks(plan, rf.ranks());
}

// The shared engine-aggregation tail (dyn reorder/fixes, key rendering,
// busy_cell_us, resolver relabel), matching the canonical layout byte-for-byte.
// `r` arrives
// from agg_finalize as [keys, value specs, text specs, dyn]; `dyn_specs` are
// the AggState's dyn side-table reductions.
dataframe::DataFrame finalize_engine_frame(
    dataframe::DataFrame r, const ViewPlan& plan,
    const std::vector<dataframe::AggDynSpec>& dyn_specs) {
    const std::size_t ng = plan.group_by.size();
    const std::size_t off = plan.time_bucket_us > 0 ? 1 : 0;
    std::vector<std::string> key_names(ng);
    std::vector<char> key_transformed(ng, 0);
    for (std::size_t j = 0; j < ng; ++j) {
        key_names[j] = group_col_name(plan.group_by[j]);
        key_transformed[j] =
            plan.group_by[j].transform != GroupKey::Transform::None ? 1 : 0;
    }
    std::optional<std::size_t> cat_pos;
    for (std::size_t j = 0; j < ng; ++j)
        if (plan.group_by[j].kind == GroupKey::Kind::Cat && !key_transformed[j])
            cat_pos = j;
    std::size_t n_plan_value = 0, n_plan_text = 0;
    for (const auto& s : plan.agg) {
        if (s.op == AggOp::ArgMax || s.op == AggOp::SetUnion)
            ++n_plan_text;
        else
            ++n_plan_value;
    }
    if (plan.agg.empty()) n_plan_value = 1;

    // agg_finalize appends dyn last; the canonical layout slots it between the
    // value and text columns. Rotate the [value..end) tail so [text, dyn]
    // becomes [dyn,
    // text], then build the post-finalize dyn fixes from the reductions.
    const std::size_t dyn_count =
        r.columns.size() - (off + ng) - (n_plan_value + n_plan_text);
    const std::size_t dyn_at = off + ng + n_plan_value;
    if (dyn_count && n_plan_text) {
        const auto first = static_cast<std::ptrdiff_t>(dyn_at);
        const auto mid = static_cast<std::ptrdiff_t>(dyn_at + n_plan_text);
        const auto last =
            static_cast<std::ptrdiff_t>(dyn_at + n_plan_text + dyn_count);
        std::rotate(r.names.begin() + first, r.names.begin() + mid,
                    r.names.begin() + last);
        std::rotate(r.columns.begin() + first, r.columns.begin() + mid,
                    r.columns.begin() + last);
    }
    std::vector<DynFix> dyn;
    for (std::size_t k = 0; k < dyn_count && !dyn_specs.empty(); ++k) {
        const dataframe::AggDynSpec& sp = dyn_specs[k % dyn_specs.size()];
        dyn.push_back({r.names[dyn_at + k], sp.op == dataframe::AggOp::Pct,
                       sp.op == dataframe::AggOp::Count});
    }
    const std::size_t n_value_cols = n_plan_value + dyn_count;

    if (off) r.names[0] = "time_bucket";
    if (cat_pos) r.names[off + *cat_pos] = "cat";
    for (std::size_t j = 0; j < ng; ++j) {
        const GroupKey::Kind k = plan.group_by[j].kind;
        if (key_transformed[j] || k == GroupKey::Kind::Arg ||
            k == GroupKey::Kind::Field)
            r.names[off + j] = key_names[j];
    }
    for (std::size_t i = 0; i < off + ng; ++i)
        r.columns[i] = key_column_to_string(r.columns[i]);

    const bool occ_cell_col =
        std::any_of(plan.agg.begin(), plan.agg.end(), [](const AggSpec& s) {
            return s.op == AggOp::Busy || s.op == AggOp::Concurrency ||
                   s.op == AggOp::Utilization;
        });
    if (occ_cell_col) {
        const std::int64_t nrows = r.num_rows();
        std::vector<std::int64_t> cell(
            static_cast<std::size_t>(nrows),
            static_cast<std::int64_t>(plan.occ_cell_us));
        const std::size_t at = off + ng + n_value_cols;
        r.names.insert(r.names.begin() + static_cast<std::ptrdiff_t>(at),
                       "busy_cell_us");
        r.columns.insert(r.columns.begin() + static_cast<std::ptrdiff_t>(at),
                         dataframe::Series::flat_i64(cell.data(), nrows));
    }

    bool needs_resolver = false;
    for (std::size_t j = 0; j < ng; ++j)
        needs_resolver =
            needs_resolver ||
            (key_is_resolved(plan.group_by[j].kind) && !key_transformed[j]);
    if (needs_resolver) {
        const GroupResolver* resolver = ensure_resolver(plan);
        for (std::size_t j = 0; j < ng; ++j) {
            if (!key_is_resolved(plan.group_by[j].kind) || key_transformed[j])
                continue;
            const std::size_t idx = off + j;
            r.columns[idx] = resolve_key_column(r.columns[idx], *resolver,
                                                plan.group_by[j].kind);
            r.names[idx] = key_names[j];
        }
    }

    for (const DynFix& d : dyn) {
        if (!d.pct) continue;
        const std::int64_t ci = r.column_index(d.out);
        if (ci < 0) continue;
        dataframe::Series& col = r.columns[static_cast<std::size_t>(ci)];
        if (col.type() != dataframe::TypeId::Float64) continue;
        const std::int64_t n = col.length();
        const double* src = col.data<double>();
        std::vector<double> vals(static_cast<std::size_t>(n));
        for (std::int64_t i = 0; i < n; ++i)
            vals[static_cast<std::size_t>(i)] =
                std::isnan(src[i]) ? 0.0 : src[i];
        col = dataframe::Series::flat_f64(vals.data(), n);
    }
    for (const DynFix& d : dyn) {
        if (!d.count) continue;
        const std::int64_t ci = r.column_index(d.out);
        if (ci < 0) continue;
        dataframe::Series& col = r.columns[static_cast<std::size_t>(ci)];
        if (col.type() != dataframe::TypeId::Int64) continue;
        const std::int64_t n = col.length();
        const std::int64_t* src = col.data<std::int64_t>();
        std::vector<double> vals(static_cast<std::size_t>(n));
        for (std::int64_t i = 0; i < n; ++i)
            vals[static_cast<std::size_t>(i)] = static_cast<double>(src[i]);
        col = dataframe::Series::flat_f64(vals.data(), n);
    }
    return r;
}

}  // namespace

std::string agg_value_base_field(const std::string& value_name) {
    if (value_name == SCALED_TS_COL) return "ts";
    if (value_name == SCALED_DUR_COL) return "dur";
    if (value_name == SCALED_TE_COL) return "te";
    return value_name;
}

dataframe::DataFrame finalize_engine_result(const dataframe::AggState& st,
                                            const ViewPlan& plan) {
    std::vector<std::string> names;
    names.reserve(1 + plan.group_by.size());
    if (plan.time_bucket_us > 0) names.push_back("time_bucket");
    for (const auto& gk : plan.group_by) names.push_back(group_col_name(gk));
    dataframe::DataFrame r = dataframe::agg_finalize(st, names);
    return finalize_engine_frame(std::move(r), plan, build_dyn_specs(plan));
}

AggInputSpec make_agg_input_spec(const ViewPlan& plan) {
    AggInputSpec spec;
    const bool has_bucket = plan.time_bucket_us > 0;
    const bool has_occ =
        std::any_of(plan.agg.begin(), plan.agg.end(),
                    [](const AggSpec& s) { return is_occupancy_op(s.op); });

    std::vector<std::string> key_fields;
    key_fields.reserve(plan.group_by.size());
    for (const GroupKey& gk : plan.group_by)
        key_fields.push_back(key_group_field(gk));

    // A scaled value field (ts/dur/te) routes through a hidden pre-scaled
    // column when time_scale is non-identity and the raw scan is read unscaled.
    // Occupancy always reads raw ts/dur, so it is excluded and never rescaled.
    const bool has_scaled_value_agg =
        std::any_of(plan.agg.begin(), plan.agg.end(), [](const AggSpec& s) {
            return !is_occupancy_op(s.op) &&
                   (is_scaled_field(s.field) || is_scaled_field(s.by));
        });
    const bool needs_value_scale =
        plan.time_scale != 1.0 && (has_bucket || has_scaled_value_agg);
    auto scaled_name = [&](const std::string& f) -> std::string {
        if (!needs_value_scale) return f;
        if (f == "ts") return SCALED_TS_COL;
        if (f == "dur") return SCALED_DUR_COL;
        if (f == "te") return SCALED_TE_COL;
        return f;
    };

    // Auto-discovered numeric args stream as the AggState dyn side-table
    // (per-morsel dyn columns), so no name pre-scan and no dyn gaggs; the
    // reductions and the raw scan's dyn emission both key off the same specs.
    spec.dyn_specs = build_dyn_specs(plan);
    spec.emit_dyn = plan.auto_numeric_metrics;
    spec.dyn_prefix = std::string(AGG_NUM_ARG_PREFIX);

    // Fixed gaggs in [value, text] order (dyn is separate); the engine emits
    // columns in gagg order.
    std::vector<dataframe::GroupAgg> text_gaggs;
    if (plan.agg.empty()) {
        dataframe::GroupAgg g;
        g.op = dataframe::Agg::Count;
        g.out = agg_col_name(AggSpec(AggOp::Count));
        spec.gaggs.push_back(std::move(g));
    } else {
        for (const auto& s : plan.agg) {
            dataframe::GroupAgg g = to_group_agg(s);
            // Occupancy reads raw ts/dur (never rescaled); every other value
            // agg over a scaled field routes to its pre-scaled column.
            if (is_occupancy_op(s.op)) {
                g.param = static_cast<double>(plan.occ_cell_us);
            } else {
                g.column = scaled_name(g.column);
                g.by = scaled_name(g.by);
            }
            if (s.op == AggOp::ArgMax || s.op == AggOp::SetUnion)
                text_gaggs.push_back(std::move(g));
            else
                spec.gaggs.push_back(std::move(g));
        }
    }
    spec.gaggs.insert(spec.gaggs.end(),
                      std::make_move_iterator(text_gaggs.begin()),
                      std::make_move_iterator(text_gaggs.end()));

    // A fixed select list keeps every streamed morsel's columns identical, so
    // the streaming group_by (which resolves columns once against the schema)
    // cannot misalign. A group key selects the field it folds on (a hash for a
    // resolved-name key), not the resolved output column.
    spec.select = key_fields;
    auto add_field = [&](const std::string& f) {
        if (f.empty()) return;
        if (std::find(spec.select.begin(), spec.select.end(), f) ==
            spec.select.end())
            spec.select.push_back(f);
    };
    for (const auto& s : plan.agg) {
        // A derived value/by field (size/te) selects its typed derived column.
        add_field(value_select_token(s.field));
        add_field(value_select_token(s.by));
    }
    if (has_bucket) add_field("ts");
    if (has_occ) {
        add_field("ts");
        add_field("dur");
    }

    // build_row_frame would pre-scale+round ts/dur; bucketing/occupancy/scaled
    // aggs need the raw values and reapply time_scale below as the fold does
    // (unrounded), so those read unscaled here.
    spec.base_time_scale =
        (has_bucket || has_occ || needs_value_scale) ? 1.0 : plan.time_scale;

    // Group-key transforms coarsen the key, so they apply before the group-by,
    // matching resolve_group_keys (resolve_group_value then
    // apply_group_transform).
    std::vector<char> key_transformed(plan.group_by.size(), 0);
    std::vector<std::string> tf_col(plan.group_by.size());
    for (std::size_t i = 0; i < plan.group_by.size(); ++i) {
        const GroupKey& gk = plan.group_by[i];
        if (gk.transform == GroupKey::Transform::None) continue;
        tf_col[i] = "__view_agg_engine_tf_" + std::to_string(i);
        key_transformed[i] = 1;
        spec.transforms.push_back({gk, key_fields[i], tf_col[i]});
        if (key_is_resolved(gk.kind)) spec.transform_wants_resolver = true;
    }

    spec.group_key_names = key_fields;
    for (std::size_t i = 0; i < plan.group_by.size(); ++i)
        if (key_transformed[i]) spec.group_key_names[i] = tf_col[i];
    // cat lowercases for grouping only, but a value agg (SetUnion(cat)) still
    // needs the raw case, so the lowered key goes to a hidden column.
    for (std::size_t i = 0; i < plan.group_by.size(); ++i) {
        if (plan.group_by[i].kind != GroupKey::Kind::Cat) continue;
        if (key_transformed[i]) continue;  // transform path lowercased it
        spec.group_key_names[i] = CAT_KEY_COL;
        spec.cat_lower_src = key_fields[i];
    }

    // Bucket key: match agg_fold.h's fold_event_over exactly. `ts` is raw; the
    // fold applies time_scale, floors (ts*scale - origin)/interval toward -inf,
    // rescales by the interval width, and shifts back by origin.
    if (has_bucket) {
        spec.bucket_ts_src = "ts";
        spec.bucket_scale = plan.time_scale;
        spec.bucket_interval = static_cast<double>(plan.time_bucket_us);
        spec.bucket_w = static_cast<std::int64_t>(plan.time_bucket_us);
        spec.bucket_origin = static_cast<std::int64_t>(plan.bucket_origin_us);
        spec.group_key_names.insert(spec.group_key_names.begin(),
                                    BUCKET_KEY_COL);
    }

    // Value fields that need the same unrounded time_scale (agg_fold.h's
    // field_scaled: ts/dur/te). Only the columns an agg spec references and
    // that the select actually carries are rescaled.
    if (needs_value_scale) {
        spec.value_scale = plan.time_scale;
        bool need_ts = false, need_dur = false, need_te = false;
        for (const auto& s : plan.agg) {
            if (is_occupancy_op(s.op)) continue;
            need_ts = need_ts || s.field == "ts" || s.by == "ts";
            need_dur = need_dur || s.field == "dur" || s.by == "dur";
            need_te = need_te || s.field == "te" || s.by == "te";
        }
        auto in_select = [&](const std::string& tok) {
            return std::find(spec.select.begin(), spec.select.end(), tok) !=
                   spec.select.end();
        };
        // Sources are select tokens (positional on the streaming path); the C++
        // path maps each to its frame column via canonical_row_column_name
        // (te's derived token becomes "te").
        if (need_ts && in_select("ts")) spec.scale_ts_src = "ts";
        if (need_dur && in_select("dur")) spec.scale_dur_src = "dur";
        if (need_te && in_select(value_select_token("te")))
            spec.scale_te_src = value_select_token("te");
    }

    return spec;
}

// Read numeric cell `r` of `c` (Uint64/Int64/Float64) as a double, for the
// unrounded time_scale rescale/bucket floor.
static double cell_as_double(const dataframe::Series& c, std::int64_t r) {
    switch (c.type()) {
        case dataframe::TypeId::Uint64:
            return static_cast<double>(c.data<std::uint64_t>()[r]);
        case dataframe::TypeId::Int64:
            return static_cast<double>(c.data<std::int64_t>()[r]);
        case dataframe::TypeId::Float64:
            return c.data<double>()[r];
        default:
            throw DFTUtilsException::cat(
                ErrorCode::INTERNAL,
                "agg engine: unexpected numeric column type");
    }
}

static void append_transform_columns(
    dataframe::DataFrame& frame,
    const std::vector<AggInputSpec::Transform>& transforms,
    const GroupResolver* resolver) {
    const std::int64_t n = frame.num_rows();
    for (const AggInputSpec::Transform& t : transforms) {
        const dataframe::Series& src = frame.columns[static_cast<std::size_t>(
            frame.column_index(t.src_col))];
        std::vector<std::string> vals(static_cast<std::size_t>(n));
        for (std::int64_t r = 0; r < n; ++r)
            vals[static_cast<std::size_t>(r)] = apply_group_transform(
                t.gk,
                transform_key_base(t.gk, cell_to_key_string(src, r), resolver));
        frame.names.push_back(t.out_col);
        frame.columns.push_back(dataframe::Series::strings(vals));
    }
}

dataframe::DataFrame build_agg_input_frame(
    const std::vector<FoldEvent>& events,
    const dftracer::utils::StringIntern& intern, const AggInputSpec& spec,
    const GroupResolver* resolver) {
    dataframe::DataFrame f = events_to_frame(
        events, intern,
        ColumnSpec{spec.select, spec.base_time_scale, nullptr, spec.emit_dyn});
    const std::int64_t n = f.num_rows();

    append_transform_columns(f, spec.transforms, resolver);

    // cat/bucket/scale sources are select tokens; map each to its built frame
    // column (te's derived token resolves to "te").
    auto col_by_token =
        [&](const std::string& tok) -> const dataframe::Series& {
        return f.columns[static_cast<std::size_t>(
            f.column_index(canonical_row_column_name(tok)))];
    };

    if (!spec.cat_lower_src.empty()) {
        const dataframe::Series& src = col_by_token(spec.cat_lower_src);
        std::vector<std::string> vals(static_cast<std::size_t>(n));
        std::vector<std::uint8_t> vbits((static_cast<std::size_t>(n) + 7) / 8,
                                        0);
        bool any_null = false;
        for (std::int64_t r = 0; r < n; ++r) {
            if (src.is_null(r)) {
                any_null = true;
                continue;
            }
            std::string s(src.string_at(r));
            for (char& ch : s)
                ch = static_cast<char>(
                    ::tolower(static_cast<unsigned char>(ch)));
            vals[static_cast<std::size_t>(r)] = std::move(s);
            vbits[static_cast<std::size_t>(r) >> 3] |=
                static_cast<std::uint8_t>(1u << (r & 7));
        }
        f.names.emplace_back(CAT_KEY_COL);
        if (!any_null) {
            f.columns.push_back(dataframe::Series::strings(vals));
        } else {
            std::vector<std::string_view> views(vals.begin(), vals.end());
            f.columns.push_back(dataframe::Series::strings(
                std::span<const std::string_view>(views), vbits.data()));
        }
    }

    if (!spec.bucket_ts_src.empty()) {
        const dataframe::Series& ts = col_by_token(spec.bucket_ts_src);
        std::vector<std::int64_t> b(static_cast<std::size_t>(n));
        for (std::int64_t r = 0; r < n; ++r) {
            const double rel = cell_as_double(ts, r) * spec.bucket_scale -
                               static_cast<double>(spec.bucket_origin);
            const auto fl = static_cast<std::int64_t>(
                std::floor(rel / spec.bucket_interval));
            b[static_cast<std::size_t>(r)] =
                fl * spec.bucket_w + spec.bucket_origin;
        }
        f.names.emplace_back(BUCKET_KEY_COL);
        f.columns.push_back(dataframe::Series::flat_i64(b.data(), n));
    }

    auto scale_into = [&](const std::string& src_tok, const char* out) {
        if (src_tok.empty()) return;
        const dataframe::Series& c = col_by_token(src_tok);
        std::vector<double> vals(static_cast<std::size_t>(n));
        std::vector<std::uint8_t> vbits((static_cast<std::size_t>(n) + 7) / 8,
                                        0);
        bool any_null = false;
        for (std::int64_t r = 0; r < n; ++r) {
            if (c.is_null(r)) {
                any_null = true;
                continue;
            }
            vals[static_cast<std::size_t>(r)] =
                cell_as_double(c, r) * spec.value_scale;
            vbits[static_cast<std::size_t>(r) >> 3] |=
                static_cast<std::uint8_t>(1u << (r & 7));
        }
        f.names.emplace_back(out);
        f.columns.push_back(dataframe::Series::flat_f64(
            vals.data(), n, any_null ? vbits.data() : nullptr));
    };
    scale_into(spec.scale_ts_src, SCALED_TS_COL);
    scale_into(spec.scale_dur_src, SCALED_DUR_COL);
    scale_into(spec.scale_te_src, SCALED_TE_COL);

    return f;
}

coro::CoroTask<EnginePrep> prepare_engine_group(const ViewPlan& plan) {
    // A Rank key groups on pid and relabels to the PR-metadata rank; harvest
    // that map into the resolver before the scan.
    if (std::any_of(
            plan.group_by.begin(), plan.group_by.end(),
            [](const GroupKey& gk) { return gk.kind == GroupKey::Kind::Rank; }))
        co_await harvest_ranks(plan);

    AggInputSpec spec = make_agg_input_spec(plan);

    auto next = std::make_shared<ViewPlan>(plan);
    next->group_by.clear();
    next->agg.clear();
    // auto_numeric_metrics stays off here: it would make the raw view a non-row
    // query (is_row_query), so the ViewSource would buffer/re-aggregate instead
    // of streaming. The dyn emission is signalled to the ViewSource directly.
    next->auto_numeric_metrics = false;
    next->numeric_arg_aggs.clear();
    next->sort_col.clear();
    next->topk_col.clear();
    next->offset = 0;
    next->limit = 0;
    next->select = spec.select;
    next->schema.reset();
    next->resolver.reset();
    next->time_bucket_us = 0;
    next->bucket_origin_us = 0;
    next->bucket_origin_min = false;
    next->time_scale = spec.base_time_scale;

    View raw(std::move(next));
    dataframe::LazyFrame lf =
        dataframe::LazyFrame::scan(
            std::make_shared<ViewSource>(raw, plan.auto_numeric_metrics))
            .memory_budget(plan.memory_budget);

    // Group-key transforms: the engine has no dirname/basename/bucket string
    // expr, so materialize the raw scan and build each transformed key column
    // in C++ (build_agg_input_frame's transform step over the whole frame),
    // then group over the in-memory frame.
    if (!spec.transforms.empty()) {
        dataframe::DataFrame frame = co_await lf.collect();
        const GroupResolver* resolver =
            spec.transform_wants_resolver ? ensure_resolver(plan) : nullptr;
        append_transform_columns(frame, spec.transforms, resolver);
        lf =
            dataframe::lazy(std::move(frame)).memory_budget(plan.memory_budget);
    }

    // Hidden columns index their source by its select position: expr_col is
    // positional, and the select columns keep positions 0..N-1 in both the
    // streaming morsel and the re-lazied transform frame.
    auto col_index = [&](const std::string& tok) {
        const auto it = std::find(spec.select.begin(), spec.select.end(), tok);
        return static_cast<std::int32_t>(it - spec.select.begin());
    };

    if (!spec.cat_lower_src.empty())
        lf = lf.with_column(CAT_KEY_COL,
                            dataframe::expr_lower(dataframe::expr_col(
                                col_index(spec.cat_lower_src))));

    if (!spec.bucket_ts_src.empty()) {
        dataframe::Expr ts_d = dataframe::expr_cast(
            dataframe::TypeId::Float64,
            dataframe::expr_col(col_index(spec.bucket_ts_src)));
        dataframe::Expr rel =
            ts_d * dataframe::expr_lit(spec.bucket_scale) -
            dataframe::expr_lit(static_cast<double>(spec.bucket_origin));
        dataframe::Expr floored = dataframe::expr_unary(
            dataframe::UnaryOp::Floor,
            rel / dataframe::expr_lit(spec.bucket_interval));
        dataframe::Expr bucket =
            dataframe::expr_cast(dataframe::TypeId::Int64, floored) *
                dataframe::expr_lit(spec.bucket_w) +
            dataframe::expr_lit(spec.bucket_origin);
        lf = lf.with_column(BUCKET_KEY_COL, bucket);
    }

    auto scale_col = [&](const std::string& src_name, const char* out) {
        if (src_name.empty()) return;
        lf = lf.with_column(out, dataframe::expr_cast(
                                     dataframe::TypeId::Float64,
                                     dataframe::expr_col(col_index(src_name))) *
                                     dataframe::expr_lit(spec.value_scale));
    };
    scale_col(spec.scale_ts_src, SCALED_TS_COL);
    scale_col(spec.scale_dur_src, SCALED_DUR_COL);
    scale_col(spec.scale_te_src, SCALED_TE_COL);

    co_return EnginePrep{std::move(lf), std::move(spec.group_key_names),
                         std::move(spec.gaggs), std::move(spec.dyn_specs),
                         std::move(spec.dyn_prefix)};
}

coro::CoroTask<dataframe::DataFrame> run_collect_via_engine(
    const ViewPlan& plan_in) {
    const ViewPlan plan = resolve_bucket_origin(plan_in);
    ensure_schema(plan);

    // The rollup carries occupancy delta-maps (bootstrap/tier do not), so it is
    // tried first and is the only fast path that can serve occupancy.
    if (auto df = try_serve_rollup(plan)) co_return std::move(*df);
    {
        dataframe::AggStatePtr served;
        if (co_await try_serve_aggregate_no_scan(plan, served))
            co_return finalize_engine_result(*served, plan);
    }

    EnginePrep ep = co_await prepare_engine_group(plan);

    // materialize() persists the AggState partials as a rollup (opt-in); a
    // paginated result is never cached.
    if (plan.materialize && !plan.limit && !plan.offset) {
        auto state = co_await ep.lf->collect_group_state(
            ep.group_key_names, ep.gaggs, ep.dyn_specs, ep.dyn_prefix);
        const std::string rdir = rollup_index_path(plan);
        if (!rdir.empty()) {
            try {
                auto db = open_rollup_db(
                    rdir, rocksdb::RocksDatabase::OpenMode::ReadWrite);
                if (db)
                    persist_rollup(*db, plan_signature(plan),
                                   rest_signature(plan), plan.time_bucket_us,
                                   plan.group_by, *state);
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_WARN("rollup materialize skipped: %s",
                                        e.what());
            }
        }
        co_return finalize_engine_result(*state, plan);
    }

    // A global aggregation (no group_by, no time_bucket) is one group; the
    // streaming group_by wants at least one key column, so fold it into a
    // single AggState (empty key list) and finalize that.
    if (plan.group_by.empty() && plan.time_bucket_us == 0) {
        auto state = co_await ep.lf->collect_group_state(
            ep.group_key_names, ep.gaggs, ep.dyn_specs, ep.dyn_prefix);
        co_return finalize_engine_result(*state, plan);
    }

    dataframe::DataFrame r = co_await ep.lf
                                 ->group_by(ep.group_key_names, ep.gaggs,
                                            ep.dyn_specs, ep.dyn_prefix)
                                 .collect();
    co_return finalize_engine_frame(std::move(r), plan, ep.dyn_specs);
}

coro::CoroTask<dataframe::AggStatePtr> build_engine_agg_state(
    const ViewPlan& plan_in) {
    const ViewPlan plan = resolve_bucket_origin(plan_in);
    ensure_schema(plan);
    EnginePrep ep = co_await prepare_engine_group(plan);
    co_return co_await ep.lf->collect_group_state(ep.group_key_names, ep.gaggs,
                                                  ep.dyn_specs, ep.dyn_prefix);
}

}  // namespace dftracer::utils::trace::views::detail
