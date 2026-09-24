#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGG_ENGINE_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGG_ENGINE_H

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <optional>
#include <string>
#include <vector>

// View -> dataframe engine aggregation convergence: every group_by/agg (and
// global) View aggregation runs through the dataframe engine's streaming
// group_by. run_collect_via_engine itself is declared (as a friend of View) in
// view.h; this header adds the finalize/prepare helpers.
namespace dftracer::utils::trace::views::detail {

/// Finalize a raw engine AggState (key layout [time_bucket?, group_by...] with
/// value/text/dyn specs in engine order) to the result DataFrame, applying the
/// same key rendering, resolver relabel, busy_cell_us column and dyn column
/// fixes as the streaming scan path - so a rollup-served result matches a fresh
/// scan byte-for-byte. `plan` supplies the grouping/resolver/occupancy context.
/// The columns an aggregation of `plan` produces, from finalizing an empty
/// state through the same path, so no trace is read. A column whose type
/// follows its input's (sum, sumsq, min, max, argmax) is Unknown.
/// std::nullopt for auto_numeric_metrics, whose columns depend on the data.
std::optional<dftracer::utils::dataframe::Schema> aggregated_output_schema(
    const ViewPlan& plan);

dftracer::utils::dataframe::DataFrame finalize_engine_result(
    const dftracer::utils::dataframe::AggState& state, const ViewPlan& plan);

/// Scan `plan` through the engine group-by and return the mergeable partial
/// (fine-grain, key layout [time_bucket?, group_by...]) before finalizing. The
/// unit persisted as a rollup and merged across distributed ranks; finalize
/// with finalize_engine_result or coarsen with agg_regroup.
coro::CoroTask<dftracer::utils::dataframe::AggStatePtr> build_engine_agg_state(
    const ViewPlan& plan);

/// One dyn (auto_numeric_metrics) output column needing a post-finalize fix: a
/// Pct on an absent arg reads an empty sketch (NaN -> 0), a Count is folded as
/// a present-count (Int64) but the dyn convention is Float64.
struct DynFix {
    std::string out;
    bool pct = false;
    bool count = false;
};

/// The engine group-by inputs for a bucket-resolved plan: the raw scan
/// LazyFrame with its hidden key/scale columns, the group key column names, the
/// fixed gaggs in [value, text] order, and the name-keyed dyn side-table
/// reductions (auto_numeric_metrics) plus the column-name tag they carry.
struct EnginePrep {
    std::optional<dftracer::utils::dataframe::LazyFrame> lf;
    std::vector<std::string> group_key_names;
    std::vector<dftracer::utils::dataframe::GroupAgg> gaggs;
    std::vector<dftracer::utils::dataframe::AggDynSpec> dyn_specs;
    std::string dyn_prefix;
};

class GroupResolver;

/// The one derivation of a ViewPlan's aggregation into engine terms, shared by
/// prepare_engine_group (builds the LazyFrame via exprs) and
/// build_agg_input_frame (builds the same columns from an event chunk in C++).
/// Holds the raw-scan select and base time_scale, the final key column names,
/// the fixed gaggs (value then text), the dyn reductions, and the key/value
/// transforms both paths apply on top of the base frame.
struct AggInputSpec {
    std::vector<std::string> select;  ///< build_row_frame select tokens
    /// The plan's computed columns, each expression positional against
    /// `select`, appended (or replacing a same-named column) before grouping.
    struct Computed {
        std::string name;
        dftracer::utils::dataframe::Expr expr;
    };
    std::vector<Computed> computed;
    double base_time_scale = 1.0;  ///< time_scale passed to build_row_frame
    bool emit_dyn = false;         ///< append build_dyn_numeric_columns

    std::vector<std::string> group_key_names;  ///< final key column names
    std::vector<dftracer::utils::dataframe::GroupAgg> gaggs;
    std::vector<dftracer::utils::dataframe::AggDynSpec> dyn_specs;
    std::string dyn_prefix;

    /// cat key(s) lowercased for grouping only; the base column to lower into
    /// the hidden CAT_KEY_COL, or empty when no non-transformed cat key.
    std::string cat_lower_src;
    /// Group-key value transforms (dirname/basename/lower/bucket): read
    /// `src_col` from the base frame, resolve+transform each cell into
    /// `out_col`.
    struct Transform {
        GroupKey gk;
        std::string src_col;
        std::string out_col;
    };
    std::vector<Transform> transforms;
    bool transform_wants_resolver = false;

    /// Time-bucket floor over the base ts column into the hidden
    /// BUCKET_KEY_COL; empty `bucket_ts_src` means no bucket.
    std::string bucket_ts_src;
    double bucket_scale = 1.0;
    double bucket_interval = 0.0;
    std::int64_t bucket_w = 0;
    std::int64_t bucket_origin = 0;

    /// Unrounded ts/dur/te rescale (agg_fold.h field_scaled), each into its
    /// hidden SCALED_* column; an empty source name skips that field.
    double value_scale = 1.0;
    std::string scale_ts_src, scale_dur_src, scale_te_src;
};

/// Derive the shared aggregation input spec for a bucket-resolved,
/// schema-ensured `plan`.
AggInputSpec make_agg_input_spec(const ViewPlan& plan);

/// Build the engine group-by input frame for one event chunk exactly as the
/// streaming path's morsel-plus-exprs would: build_row_frame over
/// `spec.select`, the optional dyn columns, then the cat-lower /
/// group-transform / time-bucket / value-scale columns `spec` describes.
/// `resolver` is required only when `spec.transform_wants_resolver`. The result
/// feeds agg_accumulate_chunk with `spec.group_key_names`, the gagg value
/// names, and `spec.dyn_prefix`.
dftracer::utils::dataframe::DataFrame build_agg_input_frame(
    const std::vector<FoldEvent>& events,
    const dftracer::utils::StringIntern& intern, const AggInputSpec& spec,
    const GroupResolver* resolver);

/// The base trace field a group-agg value column name reduces over, inverting
/// the scaled-column rename make_agg_input_spec applies (SCALED_{TS,DUR,TE} map
/// back to ts/dur/te); any other name is returned unchanged. The tier's seed
/// path uses this to fill a value column's FieldStat from the stored metric for
/// the underlying field.
std::string agg_value_base_field(const std::string& value_name);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_AGG_ENGINE_H
