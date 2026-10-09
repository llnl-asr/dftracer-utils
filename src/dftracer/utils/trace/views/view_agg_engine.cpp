#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/index/cache/rollup_store.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/trace/views/aggfold.h>
#include <dftracer/utils/trace/views/batch_bridge.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/trace/views/view_source.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iterator>
#include <limits>
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
// emits. Empty field stays empty (Count()'s neutral value column). A
// path-decoded record's field is named by its path.
std::string value_col_name(const std::string& field, bool by_path) {
    return field.empty() ? std::string()
                         : canonical_row_column_name(field, by_path);
}

// A field agg_field_typed_t/agg_field_t derives instead of reading straight
// (size = io-cat byte size, te = ts+dur). The raw scan builds it as a typed
// derived column (AGG_DERIVED_PREFIX / derived_agg_column) rather than reading
// a stored field. A path-decoded record has no derived fields.
bool is_derived_field(const std::string& f, bool by_path) {
    return !by_path && (f == "size" || f == "te");
}

// The frame column name a value/by field's group agg reads: a derived field
// (size/te) keeps its own name (build_row_frame emits it under that name from
// the derived token), any other field maps through value_col_name.
std::string value_col(const std::string& field, bool by_path) {
    return is_derived_field(field, by_path) ? field
                                            : value_col_name(field, by_path);
}

// The raw-scan select token that produces a value/by field's column: a derived
// field routes through AGG_DERIVED_PREFIX (a typed derived column), any other
// field is selected by its own name. Empty field stays empty.
std::string value_select_token(const std::string& field, bool by_path) {
    if (field.empty()) return std::string();
    return is_derived_field(field, by_path)
               ? std::string(AGG_DERIVED_PREFIX) + field
               : field;
}

bool has_occupancy(const ViewPlan& plan) {
    return std::any_of(plan.agg.begin(), plan.agg.end(),
                       [](const AggSpec& s) { return is_occupancy_op(s.op); });
}

// The raw [lo, hi) a windowed occupancy plan clips its intervals to; ts is
// integral, so the window's start rule lo <= ts < hi holds on the ceilings.
std::optional<std::pair<std::uint64_t, std::uint64_t>> occupancy_window(
    const ViewPlan& plan) {
    if (!plan.time_range || !has_occupancy(plan)) return std::nullopt;
    auto edge = [](double v) {
        return v > 0 ? static_cast<std::uint64_t>(std::ceil(v))
                     : std::uint64_t{0};
    };
    return std::make_pair(edge(plan.time_range->first),
                          edge(plan.time_range->second));
}

// Occupancy over a window or in buckets is bounded by them at finalize
// (agg_clip_occupancy), which needs the whole AggState.
bool clips_occupancy(const ViewPlan& plan) {
    return (plan.time_range || plan.time_bucket_us > 0) && has_occupancy(plan);
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

dataframe::GroupAgg to_group_agg(const AggSpec& spec, bool by_path) {
    dataframe::GroupAgg g;
    g.op = to_engine_agg(spec.op);
    g.out = agg_col_name(spec);
    g.param = spec.q;
    if (spec.op == AggOp::ArgMax) {
        g.column = value_col(spec.field, by_path);
        g.by = value_col(spec.by, by_path);
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
            g.column = value_col(spec.field, by_path);
        }
    } else {
        g.column = value_col(spec.field, by_path);
    }
    return g;
}

// The final group-key column must be a String (group keys are text), but
// agg_finalize keeps a non-string key's native type (Int64/Uint64/Float64).
// Render it back to the decimal text form, keeping null keys null.
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
                vals[static_cast<std::size_t>(i)] =
                    dftracer::utils::double_text(d[i]);
            break;
        }
        default:
            throw DFTUtilsException::cat(
                ErrorCode::INTERNAL,
                "agg engine: unexpected group-key column type");
    }
    if (col.null_count() == 0) return dataframe::Series::strings(vals);
    std::vector<std::string_view> views(vals.begin(), vals.end());
    std::vector<std::uint8_t> vbits((static_cast<std::size_t>(n) + 7) / 8, 0);
    for (std::int64_t i = 0; i < n; ++i)
        if (!col.is_null(i))
            vbits[static_cast<std::size_t>(i) >> 3] |=
                static_cast<std::uint8_t>(1u << (i & 7));
    return dataframe::Series::strings(std::span<const std::string_view>(views),
                                      vbits.data());
}

// Keys whose group column is an opaque identifier the post-aggregation re-key
// pass relabels to a human-readable name: fhash/hhash for file and host keys,
// pid for Rank (the `ranks` row set keys on pid).
bool key_is_resolved(GroupKey::Kind kind) {
    return kind == GroupKey::Kind::FilePath ||
           kind == GroupKey::Kind::FileName ||
           kind == GroupKey::Kind::HostName || kind == GroupKey::Kind::Rank;
}

// The raw scan/group-by field a key groups on: fhash/hhash for a resolved
// name key (the fold groups on the hash, a bijection, and relabels to the
// resolved name only after aggregation), pid for Rank (the rank map keys on
// pid, matching agg_fold.h's append_group_dim; a path plan's entity role
// instead), a group-key-string sentinel for
// an Arg/Field key (rendered like the engine agg path, then relabeled to
// group_col_name after aggregation), else the key's own column.
// Which group keys of a path-decoded `plan` read a field of mixed JSON types;
// each keys on the value's JSON text and gives a JSON key column.
std::vector<char> json_keys(const ViewPlan& plan) {
    std::vector<char> out(plan.group_by.size(), 0);
    if (!plan_by_path(plan)) return out;
    const auto plain_field = [](const GroupKey& gk) {
        return (gk.kind == GroupKey::Kind::Field ||
                gk.kind == GroupKey::Kind::Arg) &&
               gk.transform == GroupKey::Transform::None;
    };
    if (std::none_of(plan.group_by.begin(), plan.group_by.end(), plain_field))
        return out;
    const auto json = scan::json_columns(plan);
    if (!json) return out;
    for (std::size_t j = 0; j < plan.group_by.size(); ++j)
        out[j] = plain_field(plan.group_by[j]) &&
                 json->count(plan.group_by[j].arg) != 0;
    return out;
}

std::string key_group_field(const GroupKey& gk) {
    switch (gk.kind) {
        case GroupKey::Kind::FilePath:
        case GroupKey::Kind::FileName:
            return "fhash";
        case GroupKey::Kind::HostName:
            return "hhash";
        case GroupKey::Kind::Rank:
            return gk.arg.empty() ? std::string("pid") : gk.arg;
        case GroupKey::Kind::Arg:
            return std::string(AGG_KEY_ARG_PREFIX) + gk.arg;
        case GroupKey::Kind::Field:
            return std::string(AGG_KEY_FIELD_PREFIX) + gk.arg;
        case GroupKey::Kind::Expr:
            return gk.arg;
        default:
            return group_col_name(gk);
    }
}

// Whether `gk` groups on a sentinel key column, whose null (a missing or
// JSON-null value) is its own group and stays null in the output.
bool key_is_nullable(const GroupKey& gk) {
    return gk.kind == GroupKey::Kind::Arg || gk.kind == GroupKey::Kind::Field;
}

// Resolve one already-stringified hash group-key column to its resolved name,
// matching resolve_group_keys/resolve_group_value exactly (same GroupResolver,
// same FileName-from-FilePath basename derivation).
dataframe::Series resolve_key_column(
    const dataframe::Series& hashes,
    const dftracer::utils::index::plan::GroupResolver& resolver,
    const GroupKey& gk) {
    const std::int64_t n = hashes.length();
    const bool nullable = key_is_nullable(gk) && hashes.null_count() > 0;
    std::vector<std::string> vals(static_cast<std::size_t>(n));
    std::vector<std::uint8_t> vbits(
        nullable ? (static_cast<std::size_t>(n) + 7) / 8 : 0, 0);
    for (std::int64_t i = 0; i < n; ++i) {
        if (nullable) {
            if (hashes.is_null(i)) continue;
            vbits[static_cast<std::size_t>(i) >> 3] |=
                static_cast<std::uint8_t>(1u << (i & 7));
        }
        vals[static_cast<std::size_t>(i)] =
            resolve_group_value(resolver, gk, std::string(hashes.string_at(i)));
    }
    if (!nullable) return dataframe::Series::strings(vals);
    std::vector<std::string_view> views(vals.begin(), vals.end());
    return dataframe::Series::strings(std::span<const std::string_view>(views),
                                      vbits.data());
}

// One raw group-key column cell rendered as the engine agg path builds its
// key: a String cell verbatim, an integer cell as decimal, a null cell as the
// empty string (the caller keeps a nullable key's null cell null).
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
std::string transform_key_base(
    const GroupKey& gk, std::string raw,
    const dftracer::utils::index::plan::GroupResolver* resolver) {
    if (key_is_resolved(gk.kind))
        return resolver ? resolve_group_value(*resolver, gk, raw) : raw;
    if (gk.kind == GroupKey::Kind::Cat)
        for (char& ch : raw)
            ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
    return raw;
}

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
    const std::size_t fixed = off + ng + n_plan_value + n_plan_text;
    if (r.columns.size() < fixed || r.names.size() != r.columns.size())
        throw DFTUtilsException::cat(
            ErrorCode::INTERNAL, "agg engine: the grouped result has ",
            r.columns.size(), " columns and ", r.names.size(),
            " names, the plan needs at least ", fixed);

    // agg_finalize appends dyn last; the canonical layout slots it between the
    // value and text columns. Rotate the [value..end) tail so [text, dyn]
    // becomes [dyn,
    // text], then build the post-finalize dyn fixes from the reductions.
    const std::size_t dyn_count = r.columns.size() - fixed;
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
            k == GroupKey::Kind::Field || k == GroupKey::Kind::Expr)
            r.names[off + j] = key_names[j];
    }
    const std::vector<char> json = json_keys(plan);
    for (std::size_t i = 0; i < off + ng; ++i)
        if (i < off || plan.group_by[i - off].kind != GroupKey::Kind::Expr) {
            r.columns[i] = key_column_to_string(r.columns[i]);
            if (i >= off && json[i - off])
                r.columns[i] = r.columns[i].as_json();
        }

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
        const dftracer::utils::index::plan::GroupResolver* resolver =
            ensure_resolver(plan);
        for (std::size_t j = 0; j < ng; ++j) {
            if (!key_is_resolved(plan.group_by[j].kind) || key_transformed[j])
                continue;
            const std::size_t idx = off + j;
            r.columns[idx] =
                resolve_key_column(r.columns[idx], *resolver, plan.group_by[j]);
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

std::optional<dataframe::Schema> aggregated_output_schema(
    const ViewPlan& plan_in) {
    if (plan_in.auto_numeric_metrics) return std::nullopt;
    const ViewPlan plan = resolve_bucket_origin(plan_in);
    ensure_schema(plan);
    AggInputSpec spec = make_agg_input_spec(plan);
    dataframe::LoweredGroupAggs lowered =
        dataframe::lower_group_aggs(spec.gaggs);
    dataframe::AggStatePtr st =
        dataframe::agg_new(std::move(lowered.specs), spec.dyn_specs);
    dataframe::DataFrame r = finalize_engine_result(*st, plan);
    // An aggregate that passes its input's values through (sum/min/max of an
    // integer field) takes the input's type, which only a scan settles.
    std::vector<std::string> input_typed;
    for (const GroupKey& gk : plan.group_by)
        if (gk.kind == GroupKey::Kind::Expr) input_typed.push_back(gk.arg);
    for (const AggSpec& a : plan.agg)
        if (a.op == AggOp::Sum || a.op == AggOp::SumSq || a.op == AggOp::Min ||
            a.op == AggOp::Max || a.op == AggOp::ArgMax)
            input_typed.push_back(agg_col_name(a));
    dataframe::Schema s;
    s.fields.reserve(r.columns.size());
    for (std::size_t i = 0; i < r.columns.size(); ++i) {
        const bool follows_input =
            std::find(input_typed.begin(), input_typed.end(), r.names[i]) !=
            input_typed.end();
        s.fields.push_back(dataframe::Field{
            r.names[i],
            follows_input ? dataframe::scalar(dataframe::TypeId::Unknown)
                          : r.columns[i].data_type(),
            true});
    }
    return s;
}

dataframe::DataFrame finalize_engine_result(const dataframe::AggState& st,
                                            const ViewPlan& plan) {
    std::vector<std::string> names;
    names.reserve(1 + plan.group_by.size());
    if (plan.time_bucket_us > 0) names.push_back("time_bucket");
    for (const auto& gk : plan.group_by) names.push_back(group_col_name(gk));
    if (clips_occupancy(plan) && dataframe::agg_num_groups(st) > 0) {
        const auto win = occupancy_window(plan).value_or(std::make_pair(
            std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()));
        dataframe::AggStatePtr clipped = dataframe::agg_clip_occupancy(
            st, win.first, win.second,
            static_cast<std::int64_t>(plan.time_bucket_us), plan.time_scale,
            static_cast<std::int64_t>(plan.bucket_origin_us));
        return finalize_engine_frame(dataframe::agg_finalize(*clipped, names),
                                     plan, build_dyn_specs(plan));
    }
    dataframe::DataFrame r = dataframe::agg_finalize(st, names);
    return finalize_engine_frame(std::move(r), plan, build_dyn_specs(plan));
}

bool scans_by_overlap(const ViewPlan& plan) {
    return occupancy_window(plan).has_value();
}

AggInputSpec make_agg_input_spec(const ViewPlan& plan) {
    AggInputSpec spec;
    const bool by_path = plan_by_path(plan);
    spec.by_path = by_path;
    const bool has_bucket = plan.time_bucket_us > 0;
    const bool has_occ = has_occupancy(plan);
    // A windowed occupancy plan scans every event overlapping the window:
    // occupancy reads the clipped interval, every other aggregate only the
    // events that start in the window.
    const auto win = occupancy_window(plan);
    auto masked = [&](const std::string& tok) {
        return win && !tok.empty() ? window_token(win->first, win->second, tok)
                                   : tok;
    };
    // Path-decoded records carry their bound time and duration on the event,
    // not as "ts"/"dur" fields; an unbounded clip reads them unchanged.
    const bool role_time =
        by_path && !plan_record_schema(plan).roles.time.empty();
    const auto clip_or = [&](const char* part) -> std::string {
        if (win) return clip_token(win->first, win->second, part);
        if (role_time)
            return clip_token(0, std::numeric_limits<std::uint64_t>::max(),
                              part);
        return part;
    };
    const std::string clip_ts = clip_or("ts");
    const std::string clip_dur = clip_or("dur");

    std::vector<std::string> key_fields;
    key_fields.reserve(plan.group_by.size());
    const std::vector<char> json = json_keys(plan);
    for (std::size_t j = 0; j < plan.group_by.size(); ++j)
        key_fields.push_back(json[j] ? std::string(AGG_KEY_JSON_PREFIX) +
                                           plan.group_by[j].arg
                                     : key_group_field(plan.group_by[j]));

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
        auto computed_name = [&](const std::string& f) {
            return !f.empty() &&
                   std::any_of(
                       plan.computed.begin(), plan.computed.end(),
                       [&](const ComputedColumn& c) { return c.name == f; });
        };
        for (const auto& s : plan.agg) {
            dataframe::GroupAgg g = to_group_agg(s, by_path);
            // A computed column is read under its own name, not a row column's.
            if (computed_name(s.field)) g.column = s.field;
            if (computed_name(s.by)) g.by = s.by;
            // Occupancy reads raw ts/dur (never rescaled); every other value
            // agg over a scaled field routes to its pre-scaled column.
            if (is_occupancy_op(s.op)) {
                g.param = static_cast<double>(plan.occ_cell_us);
                g.column = clip_ts;
                g.by = clip_dur;
            } else {
                g.column = scaled_name(g.column);
                g.by = scaled_name(g.by);
                if (win) {
                    if (g.op == dataframe::Agg::Count) {
                        g.op = dataframe::Agg::CountValid;
                        g.column = window_token(win->first, win->second, "");
                    }
                    auto mask_col = [&](const std::string& field,
                                        std::string& col) {
                        if (field.empty() ||
                            (needs_value_scale && is_scaled_field(field)))
                            return;
                        col = masked(computed_name(field)
                                         ? field
                                         : value_select_token(field, by_path));
                    };
                    mask_col(s.field, g.column);
                    mask_col(s.by, g.by);
                }
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
    // resolved-name key), not the resolved output column. A computed column is
    // not an event field: its inputs are selected instead.
    auto is_computed = [&](const std::string& f) {
        return std::any_of(
            plan.computed.begin(), plan.computed.end(),
            [&](const ComputedColumn& c) { return c.name == f; });
    };
    auto add_select = [&](const std::string& f) {
        if (f.empty()) return;
        if (std::find(spec.select.begin(), spec.select.end(), f) ==
            spec.select.end())
            spec.select.push_back(f);
    };
    auto add_field = [&](const std::string& f) {
        if (!is_computed(f)) add_select(f);
    };
    for (const std::string& f : key_fields) add_field(f);
    for (const ComputedColumn& c : plan.computed)
        for (const std::string& in : c.inputs) add_select(in);
    std::set<std::string> masked_computed;
    for (const auto& s : plan.agg) {
        if (win && is_occupancy_op(s.op)) continue;
        if (win && s.op == AggOp::Count && s.field.empty())
            add_select(window_token(win->first, win->second, ""));
        // A derived value/by field (size/te) selects its typed derived column.
        for (const std::string* f : {&s.field, &s.by}) {
            if (win && is_computed(*f))
                masked_computed.insert(*f);
            else
                add_field(masked(value_select_token(*f, by_path)));
        }
    }
    if (has_bucket) add_field(clip_ts);
    if (has_occ) {
        add_field(clip_ts);
        add_field(clip_dur);
    }

    auto add_computed = [&](const ComputedColumn& c, bool mask) {
        std::vector<std::int32_t> at;
        at.reserve(c.inputs.size());
        for (const std::string& in : c.inputs) {
            const std::string tok = mask ? masked(in) : in;
            add_select(tok);
            at.push_back(static_cast<std::int32_t>(
                std::find(spec.select.begin(), spec.select.end(), tok) -
                spec.select.begin()));
        }
        spec.computed.push_back({mask ? masked(c.name) : c.name,
                                 dataframe::expr_remap_cols(c.expr, at)});
    };
    for (const ComputedColumn& c : plan.computed) {
        add_computed(c, false);
        if (masked_computed.count(c.name)) add_computed(c, true);
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
        spec.bucket_ts_src = clip_ts;
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
        if (need_ts && in_select(masked("ts")))
            spec.scale_ts_src = masked("ts");
        if (need_dur && in_select(masked("dur")))
            spec.scale_dur_src = masked("dur");
        if (need_te && in_select(masked(value_select_token("te", by_path))))
            spec.scale_te_src = masked(value_select_token("te", by_path));
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
    const dftracer::utils::index::plan::GroupResolver* resolver) {
    const std::int64_t n = frame.num_rows();
    for (const AggInputSpec::Transform& t : transforms) {
        const dataframe::Series& src = frame.columns[static_cast<std::size_t>(
            frame.column_index(t.src_col))];
        const bool nullable = key_is_nullable(t.gk) && src.null_count() > 0;
        std::vector<std::string> vals(static_cast<std::size_t>(n));
        std::vector<std::uint8_t> vbits(
            nullable ? (static_cast<std::size_t>(n) + 7) / 8 : 0, 0);
        for (std::int64_t r = 0; r < n; ++r) {
            if (nullable) {
                if (src.is_null(r)) continue;
                vbits[static_cast<std::size_t>(r) >> 3] |=
                    static_cast<std::uint8_t>(1u << (r & 7));
            }
            vals[static_cast<std::size_t>(r)] = apply_group_transform(
                t.gk,
                transform_key_base(t.gk, cell_to_key_string(src, r), resolver));
        }
        frame.names.push_back(t.out_col);
        if (!nullable) {
            frame.columns.push_back(dataframe::Series::strings(vals));
            continue;
        }
        std::vector<std::string_view> views(vals.begin(), vals.end());
        frame.columns.push_back(dataframe::Series::strings(
            std::span<const std::string_view>(views), vbits.data()));
    }
}

namespace {

// Rows of `inner` with the group-key transform columns appended to each morsel
// as it streams. Row-local, so the result equals transforming the whole frame.
class TransformSource final : public dataframe::Source {
   public:
    TransformSource(
        dataframe::LazyFrame inner, std::vector<AggInputSpec::Transform> tfs,
        std::shared_ptr<const dftracer::utils::index::plan::GroupResolver>
            resolver)
        : inner_(std::move(inner)),
          tfs_(std::move(tfs)),
          resolver_(std::move(resolver)) {
        schema_ = inner_.output_schema();
        for (const AggInputSpec::Transform& t : tfs_)
            schema_.fields.push_back(dataframe::Field{
                t.out_col, dataframe::scalar(dataframe::TypeId::String), true});
    }

    dataframe::Schema schema() const override { return schema_; }
    bool undeclared_columns() const override { return true; }

    dataframe::ScanResult scan(
        const dataframe::ScanRequest& req) const override {
        dataframe::ScanResult r;
        r.cursor = std::make_unique<Rows>(*this, req.projection);
        r.filters.assign(req.filters.size(), dataframe::Pushed::No);
        return r;
    }

   private:
    class Rows final : public dataframe::Cursor {
       public:
        Rows(const TransformSource& src, std::vector<std::string> projection)
            : batches_(src.inner_.stream()),
              tfs_(src.tfs_),
              resolver_(src.resolver_),
              projection_(std::move(projection)) {}

        coro::CoroTask<std::optional<dataframe::Morsel>> next(
            std::int64_t max_rows) override {
            for (;;) {
                if (rows_)
                    if (auto m = co_await rows_->next(max_rows)) {
                        m->dyn_state().name_ids = ids_;
                        m->dyn->intern = intern_;
                        co_return m;
                    }
                auto batch = co_await batches_.next();
                if (!batch) co_return std::nullopt;
                dataframe::DataFrame f = std::move(*batch);
                append_transform_columns(f, tfs_, resolver_.get());
                if (!projection_.empty()) f = f.select(projection_);
                ids_.clear();
                for (const std::string& n : f.names)
                    ids_.push_back(intern_->get_or_insert(n));
                rows_ = dataframe::InMemorySource(std::move(f)).scan({}).cursor;
            }
        }

       private:
        std::shared_ptr<dftracer::utils::StringIntern> intern_ =
            std::make_shared<dftracer::utils::StringIntern>();
        std::vector<std::uint32_t> ids_;
        coro::AsyncGenerator<dataframe::DataFrame> batches_;
        std::vector<AggInputSpec::Transform> tfs_;
        std::shared_ptr<const dftracer::utils::index::plan::GroupResolver>
            resolver_;
        std::vector<std::string> projection_;
        std::unique_ptr<dataframe::Cursor> rows_;
    };

    dataframe::LazyFrame inner_;
    std::vector<AggInputSpec::Transform> tfs_;
    // Shared, so the plan can run after the ViewPlan that built it is gone.
    std::shared_ptr<const dftracer::utils::index::plan::GroupResolver>
        resolver_;
    dataframe::Schema schema_;
};

}  // namespace

dataframe::DataFrame build_agg_input_frame(
    const std::vector<FoldEvent>& events,
    const dftracer::utils::StringIntern& intern, const AggInputSpec& spec,
    const dftracer::utils::index::plan::GroupResolver* resolver) {
    dataframe::DataFrame f =
        events_to_frame(events, intern,
                        ColumnSpec{spec.select, spec.base_time_scale,
                                   spec.emit_dyn, spec.by_path, nullptr});
    const std::int64_t n = f.num_rows();

    append_transform_columns(f, spec.transforms, resolver);

    if (!spec.computed.empty()) {
        std::vector<const dataframe::Series*> inputs;
        inputs.reserve(spec.select.size());
        for (const std::string& tok : spec.select)
            inputs.push_back(&f.columns[static_cast<std::size_t>(
                f.column_index(canonical_row_column_name(tok, spec.by_path)))]);
        std::vector<std::pair<std::string, dataframe::Series>> out;
        out.reserve(spec.computed.size());
        for (const AggInputSpec::Computed& c : spec.computed)
            out.emplace_back(c.name, dataframe::eval(c.expr, inputs));
        for (auto& [name, col] : out) {
            const std::int64_t at = f.column_index(name);
            if (at >= 0) {
                f.columns[static_cast<std::size_t>(at)] = std::move(col);
            } else {
                f.names.push_back(name);
                f.columns.push_back(std::move(col));
            }
        }
    }

    // cat/bucket/scale sources are select tokens; map each to its built frame
    // column (te's derived token resolves to "te").
    auto col_by_token =
        [&](const std::string& tok) -> const dataframe::Series& {
        return f.columns[static_cast<std::size_t>(
            f.column_index(canonical_row_column_name(tok, spec.by_path)))];
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

    std::shared_ptr<const ViewPlan> raw(std::move(next));
    dataframe::LazyFrame lf =
        dataframe::LazyFrame::scan(
            ViewSource::engine_scan(raw, plan.auto_numeric_metrics))
            .memory_budget(plan.memory_budget);

    // Group-key transforms: the engine has no dirname/basename/bucket string
    // expr, so each streamed morsel gets its transformed key columns in C++.
    if (!spec.transforms.empty()) {
        if (spec.transform_wants_resolver) ensure_resolver(plan);
        lf = dataframe::LazyFrame::scan(
                 std::make_shared<TransformSource>(
                     std::move(lf), spec.transforms,
                     spec.transform_wants_resolver ? plan.resolver : nullptr))
                 .memory_budget(plan.memory_budget);
    }

    for (const AggInputSpec::Computed& c : spec.computed)
        lf = lf.with_column(c.name, c.expr);

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
        const std::string rdir =
            dftracer::utils::index::cache::rollup_cache_path(plan);
        if (!rdir.empty()) plan_record_schema(plan);
        if (!rdir.empty()) {
            try {
                auto db = dftracer::utils::index::cache::open_rollup_db(
                    rdir, dftracer::utils::index::store::RocksDatabase::
                              OpenMode::ReadWrite);
                if (db)
                    dftracer::utils::index::cache::persist_rollup(
                        *db,
                        dftracer::utils::index::cache::plan_signature(plan),
                        dftracer::utils::index::cache::rest_signature(plan),
                        plan.time_bucket_us, plan.group_by, *state);
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_WARN("rollup materialize skipped: %s",
                                        e.what());
            }
        }
        co_return finalize_engine_result(*state, plan);
    }

    // A global aggregation (no group_by, no time_bucket) is one group, which
    // the streaming group_by cannot key, and clipped occupancy needs the whole
    // state; both fold into a single AggState and finalize that.
    if ((plan.group_by.empty() && plan.time_bucket_us == 0) ||
        clips_occupancy(plan)) {
        auto state = co_await ep.lf->collect_group_state(
            ep.group_key_names, ep.gaggs, ep.dyn_specs, ep.dyn_prefix);
        co_return finalize_engine_result(*state, plan);
    }

    dataframe::DataFrame r = co_await ep.lf
                                 ->group_by(ep.group_key_names, ep.gaggs,
                                            ep.dyn_specs, ep.dyn_prefix)
                                 .collect();
    co_return finalize_engine_frame(
        co_await dataframe::join_chunks(std::move(r)), plan, ep.dyn_specs);
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
