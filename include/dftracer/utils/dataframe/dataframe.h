// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_DATAFRAME_H
#define DFTRACER_UTILS_DATAFRAME_DATAFRAME_H

#include <dftracer/utils/common/config.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
struct ArrowSchema;
struct ArrowArray;
#endif

namespace dftracer::utils::dataframe {

#ifdef DFTRACER_UTILS_ENABLE_ARROW
class OwnedArrow;
#endif

/// Aggregate function selector for group_by / group_by_dynamic / pivot. First
/// and Last take the group's first / last non-null value in row order (an
/// order-independent merge, so the parallel/distributed path is exact).
enum class Agg {
    Sum,
    Min,
    Max,
    Count,
    Mean,
    Var,
    Std,
    Skew,
    Kurt,
    First,
    Last,
    Pct,
    Hist,
    ArgMax,
    SumSq,
    SetUnion,
    Busy,         ///< occupancy: exact interval-union length over (ts, dur)
    Concurrency,  ///< occupancy: sum(dur) / busy
    Utilization,  ///< occupancy: busy / makespan
    Active,       ///< occupancy: peak overlap depth
    CountValid,   ///< count of present (non-null) values in `column`, Int64
    ArgMin,       ///< `column`'s repr at the row minimizing `by`
    BitOr,        ///< bitwise OR of `column` read as u64, Uint64
    Distinct,  ///< approximate distinct count of `column` (`param` = k), Int64
    ListSorted,  ///< `column`'s reprs ordered by `by` ascending, list<string>
    TopK,  ///< `column`'s reprs at the `param` (k) largest `by`, list<string>
    BottomK,     ///< `column`'s reprs at the `param` (k) smallest `by`,
                 ///< list<string>
    ApproxTopK,  ///< heavy hitters of `column` (`param` counters),
                 ///< list<struct{value, count}>
    Sample,      ///< bottom-k-by-hash sample of `column`'s distinct reprs
                 ///< (`param` = k), list<string>
    Corr,        ///< correlation of (x = `by`, y = `column`), Float64
    CovarPop,    ///< population covariance of (x = `by`, y = `column`), Float64
    CovarSamp,   ///< sample covariance of (x = `by`, y = `column`), Float64
    RegrSlope,   ///< least-squares slope of `column` on `by`, Float64
    RegrIntercept,  ///< least-squares intercept of `column` on `by`, Float64
    RegrR2,  ///< coefficient of determination of `column` on `by`, Float64
    Prod,    ///< product of `column`'s present values, Float64
    SketchQuantile  ///< `param` quantile of the merged base64 DDSketch cells
                    ///< of a String `column`, Float64
};

/// Canonical lowercase name of `agg` (the string the C ABI accepts).
const char* to_string(Agg agg) noexcept;

/// Parse a canonical aggregate name (as `to_string` produces) into an Agg.
/// A NotFound error on an unknown name.
dstd::Result<Agg> agg_from_string(dstd::StringView name);

/// Map the public Agg selector to the engine AggOp code. Same op; the two enums
/// carry the same op under different stable values.
AggOp to_agg_op(Agg agg);
/// Inverse of to_agg_op.
Agg from_agg_op(AggOp op);

/// One aggregate in a group_by: `op` selects the function (First/Last are pivot
/// only); `column` is the value column (ignored for Count, which is the group
/// size); `out` is the result column name.
struct GroupAgg {
    Agg op = Agg::Count;
    dstd::String column;
    dstd::String out;
    double param = 0.0;  ///< Pct, SketchQuantile: quantile q in [0, 1];
                         ///< occupancy: occ_cell_us;
                         ///< Distinct/TopK/BottomK/ApproxTopK/Sample: k;
                         ///< SetUnion: nonzero asks for the typed list
    dstd::String by{};   ///< the second input column for the ops
                         ///< agg_uses_by_col() names; unused otherwise
};

class LazyFrame;         // dataframe/lazyframe.h
class GroupBy;           // below

/// A named ordered set of columns (the RecordBatch / DataChunk analogue). The
/// eager methods return a new DataFrame (move-only); projection ops share the
/// underlying column buffers zero-copy, row ops build new columns.
struct DataFrame {
    dstd::Vector<dstd::String> names;
    dstd::Vector<Series> columns;

    std::size_t num_columns() const noexcept { return columns.size(); }
    std::int64_t num_rows() const noexcept {
        return columns.empty() ? 0 : columns.front().length();
    }

    /// Index of the column named `name`, or -1 if absent.
    std::int64_t column_index(dstd::StringView name) const;
    /// A column by name, sharing its buffers zero-copy; invalid if absent.
    Series column(dstd::StringView name) const;

    /// Begin a deferred query over this frame; include dataframe/lazyframe.h.
    LazyFrame lazy() const;

    /// Yield fixed-size row slices of this frame (0 = DEFAULT_MORSEL_ROWS).
    /// Zero-copy: each chunk shares this frame's column buffers.
    coro::AsyncGenerator<DataFrame> stream(std::int64_t morsel_rows = 0) const;

    DataFrame take(const dstd::Vector<std::int64_t>& indices) const;
    DataFrame filter(const Series& mask) const;
    DataFrame slice(std::int64_t offset, std::int64_t len) const;
    DataFrame head(std::int64_t n) const;
    DataFrame tail(std::int64_t n) const;
    DataFrame reverse() const;
    /// Sort by column `name`; the errors of @ref dataframe::sort_by (a NotFound
    /// error for an absent column, an InvalidArgument error for a nested one).
    dstd::Result<DataFrame> sort_by(const dstd::String& name,
                                    bool descending = false) const;
    /// Stable lexicographic sort by several key columns (nulls last); the
    /// errors of @ref dataframe::sort_by_multi.
    dstd::Result<DataFrame> sort_by_multi(const dstd::Vector<dstd::String>& by,
                                          bool descending = false) const;
    /// Per-column direction form: `descending[i]` applies to `by[i]`; a single
    /// flag broadcasts to every key.
    dstd::Result<DataFrame> sort_by_multi(
        const dstd::Vector<dstd::String>& by,
        const dstd::Vector<bool>& descending) const;
    /// The k rows with the largest (or smallest) values of `name`; the errors
    /// of @ref dataframe::topk.
    dstd::Result<DataFrame> topk(const dstd::String& name, std::int64_t k,
                                 bool largest = true) const;

    /// Drop every row that is null in any column.
    DataFrame drop_nulls() const;
    /// Fill nulls in every column with `value` (columns whose type rejects it
    /// are left unchanged).
    DataFrame fill_null(Scalar value) const;
    /// Fill nulls with a natural C++ value (fill_null(0)); the value converts
    /// to each column's element type.
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    DataFrame fill_null(T value) const {
        return fill_null(to_scalar(value));
    }
    /// Distinct rows, keeping the first occurrence, keyed on every column or
    /// on `subset` (empty = all); the errors of @ref dataframe::unique.
    dstd::Result<DataFrame> unique(
        const dstd::Vector<dstd::String>& subset = {}) const;
    /// Alias of unique().
    dstd::Result<DataFrame> drop_duplicates(
        const dstd::Vector<dstd::String>& subset = {}) const;
    /// A deterministic n-row sample (mix64(index + seed) bottom-n).
    DataFrame sample(std::int64_t n, std::uint64_t seed = 0) const;
    /// Prepend an Int64 row-index column named `name`.
    DataFrame with_row_index(const dstd::String& name) const;
    /// Per-column summary statistics (one row per statistic).
    DataFrame describe() const;
    /// A one-row frame of each column's null count.
    DataFrame null_count() const;
    /// The whole frame as one group: any aggregate specs, one output row
    /// (group_by with no key).
    dstd::Result<DataFrame> reduce(const dstd::Vector<GroupAgg>& aggs) const;
    /// One row reducing each eligible column with `agg` under its own name
    /// (the pandas `df.sum()` family): CountValid / First / Last take every
    /// column, the other one-column aggregates the numeric ones. The grouped
    /// form is group_by(keys, reduce_specs(*this, agg, keys)). An
    /// InvalidArgument error for an aggregate that takes a second column or a
    /// parameter.
    dstd::Result<DataFrame> reduce(Agg agg) const;
    /// Bool mask (per row): true where the whole row is duplicated. An
    /// InvalidArgument error for a column type with no per-row value to key on.
    dstd::Result<Series> is_duplicated() const;
    /// Bool mask (per row): true where the whole row is unique. Errors as
    /// is_duplicated().
    dstd::Result<Series> is_unique() const;
    /// Int32 column: the part in [0, n_parts) each row lands in under a
    /// stable hash of the `keys` columns; the errors of
    /// @ref dataframe::partition_id.
    dstd::Result<Series> partition_id(const dstd::Vector<dstd::String>& keys,
                                      std::int64_t n_parts) const;

    /// Evaluate a compiled query as a per-row bit-packed Bool mask (for
    /// filter()) under three-valued logic: an UNKNOWN row (a missing column,
    /// a null cell, a type mismatch) is null and filter() drops it. An error
    /// if the predicate has no columnar lowering (pattern match, any(),
    /// ordered comparison on a string or bool column).
    dstd::Result<Series> mask(const duql::Query& q) const;

    /// Project the named columns; the errors of @ref dataframe::select.
    dstd::Result<DataFrame> select(
        const dstd::Vector<dstd::String>& names) const;
    /// Rename columns positionally; the errors of @ref dataframe::rename.
    dstd::Result<DataFrame> rename(
        const dstd::Vector<dstd::String>& new_names) const;
    DataFrame with_column(const dstd::String& name, const Series& col) const;

    /// Group rows by column `key` (first-seen order) and compute each
    /// aggregate. The errors of @ref dataframe::group_by (a NotFound error for
    /// an unknown column or op).
    dstd::Result<DataFrame> group_by(const dstd::String& key,
                                     const dstd::Vector<GroupAgg>& aggs) const;
    /// Group by N key columns (a composite key: hashed and compared
    /// column-by-column, each keeping its own type) and compute each
    /// aggregate.
    dstd::Result<DataFrame> group_by(const dstd::Vector<dstd::String>& keys,
                                     const dstd::Vector<GroupAgg>& aggs) const;
    /// The two-step form: the keys now, the aggregates on the returned
    /// @ref GroupBy (`df.group_by({"k"}).sum()`, `.agg(specs)`). Empty keys
    /// make the whole frame one group. A key the frame lacks is reported by
    /// the first aggregate, as a NotFound error.
    GroupBy group_by(dstd::Vector<dstd::String> keys = {}) const;
    /// Group by an expression key and compute each expression aggregate in one
    /// CSE'd, pruned pass (see group_agg_expr). A bare column-ref key uses that
    /// column's name for the result; a computed key is named "key". The errors
    /// of @ref group_agg_expr.
    dstd::Result<DataFrame> group_by(
        const Expr& key, const dstd::Vector<AggExprSpec>& aggs) const;
    /// Group by N expression keys; each bare column-ref key uses that column's
    /// name, a computed key is named "key<i>".
    dstd::Result<DataFrame> group_by(
        const dstd::Vector<Expr>& keys,
        const dstd::Vector<AggExprSpec>& aggs) const;

    /// Hash join with `other` on `left_on[i]` = `right_on[i]` (exact match; a
    /// null key never matches unless `nulls_equal`, which makes a null key cell
    /// match another null key cell and is refused for Cross, Lookup and Nest;
    /// each pair must share a type). Output: this
    /// frame's columns, then `other`'s except a key sharing its left key's
    /// name (emitted once); any other colliding name gets `suffix`. Matched
    /// rows keep this frame's order; Right / Outer append the unmatched right
    /// rows. Semi / Anti return this frame's columns only; Cross ignores the
    /// keys. Lookup and Nest keep each row of this frame once and compare keys
    /// by value (dftu_join_how); Nest names its list column `suffix`. Throws
    /// std::out_of_range on an absent key, std::invalid_argument on an empty
    /// or uneven key list, a key type mismatch (not for Lookup or Nest), or a
    /// Lookup key whose right rows differ or whose filled left cell holds a
    /// value.
    DataFrame join(const DataFrame& other,
                   const dstd::Vector<dstd::String>& left_on,
                   const dstd::Vector<dstd::String>& right_on,
                   JoinHow how = JoinHow::Inner,
                   const dstd::String& suffix = "_right",
                   bool nulls_equal = false) const;
    /// Join on the same-named key columns `on`.
    DataFrame join(const DataFrame& other, const dstd::Vector<dstd::String>& on,
                   JoinHow how = JoinHow::Inner,
                   const dstd::String& suffix = "_right",
                   bool nulls_equal = false) const;
    /// Compare this aggregation result with `variant` on their first `n_key`
    /// key columns: keys, `l_<m>` / `r_<m>` per metric, then `delta_<m>` and
    /// `pct_<m>` per numeric metric.
    DataFrame compare_agg(const DataFrame& variant, std::int64_t n_key) const;

    /// Reshape wide -> long: keep `id_vars`, stack `value_vars` into a
    /// `variable`/`value` column pair. `melt` is an alias. The errors of
    /// @ref dataframe::unpivot.
    dstd::Result<DataFrame> unpivot(
        const dstd::Vector<dstd::String>& id_vars,
        const dstd::Vector<dstd::String>& value_vars) const;
    dstd::Result<DataFrame> melt(
        const dstd::Vector<dstd::String>& id_vars,
        const dstd::Vector<dstd::String>& value_vars) const;

    /// Expand a List `column`: each list element becomes its own row (other
    /// columns repeated); an empty/null list yields one null row. The errors
    /// of @ref dataframe::explode.
    dstd::Result<DataFrame> explode(const dstd::String& column) const;
    /// UNNEST a List `column`: as explode, but an empty/null list drops the
    /// row unless `keep_empty`, and a List<Struct> flattens into one column
    /// per field.
    dstd::Result<DataFrame> unnest(const dstd::String& column,
                                   bool keep_empty = false) const;

    /// One-hot encode `column`: replace it with one Int8 column per distinct
    /// value (sorted), named `<column>_<value>`. The errors of
    /// @ref dataframe::to_dummies.
    dstd::Result<DataFrame> to_dummies(const dstd::String& column) const;

    /// Reshape long -> wide: rows are the distinct `index` values (sorted), one
    /// value column per distinct `columns` value (sorted), each cell `values`
    /// aggregated over the matching rows. `agg` is the collision reducer
    /// (first|last|sum|min|max|mean; mean yields Float64). The errors of
    /// @ref dataframe::pivot.
    dstd::Result<DataFrame> pivot(const dstd::String& index,
                                  const dstd::String& on,
                                  const dstd::String& values,
                                  const dstd::String& agg = "first") const;
    /// pivot with a typed collision reducer (any Agg, including First/Last).
    dstd::Result<DataFrame> pivot(const dstd::String& index,
                                  const dstd::String& on,
                                  const dstd::String& values, Agg agg) const;

    /// Tumbling/sliding time-window aggregation over an ascending Int64
    /// `time_col`: windows start at the first time floored to a multiple of
    /// `every`, stride by `every`, and each covers `[start, start + period)`
    /// (`period <= 0` means "= every"). One row per non-empty window, a leading
    /// Int64 `time_col` window-start column plus each aggregate. An
    /// InvalidArgument error when a non-null time is below the one before; the
    /// other errors of @ref dataframe::group_by_dynamic.
    dstd::Result<DataFrame> group_by_dynamic(const dstd::String& time_col,
                                             std::int64_t every,
                                             std::int64_t period,
                                             const dstd::Vector<GroupAgg>& aggs,
                                             std::int64_t origin = 0,
                                             bool origin_min = false) const;

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    /// Export this frame through the Arrow C Data Interface as a struct array
    /// with one child per column (zero copy: the exported buffers alias the
    /// columns' and are kept alive by the returned owner's release callbacks).
    /// Include <dftracer/utils/dataframe/arrow.h> for the OwnedArrow
    /// definition.
    OwnedArrow to_arrow() const;

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    /// Serialize this frame to an Arrow IPC stream (schema message + one record
    /// batch + EOS) as bytes any Arrow IPC reader can open - no pyarrow. Throws
    /// std::runtime_error on an encode failure.
    dstd::Vector<std::uint8_t> to_ipc() const;
#endif

    /// Import a STRUCT Arrow array (viewed through `schema`) as a @ref
    /// dftracer::utils::dataframe::DataFrame "DataFrame": one column per struct
    /// child, named by its child schema, wrapped zero copy. ADOPTS `array`
    /// (ownership of its buffers moves into the frame and the passed array is
    /// marked released, so the caller must not release it again); `schema` is
    /// only read. An empty frame on a non-struct or unsupported child type.
    static DataFrame from_arrow(const ArrowSchema* schema,
                                const ArrowArray* array);
#endif
};

/// The two-step group-by: a frame (shared zero-copy) and its key columns,
/// with the aggregates chosen on it. Every method is one call to
/// DataFrame::group_by(keys, specs); the family below is the pandas
/// `groupby(k).sum()` shape, each aggregate broadcast over the eligible
/// non-key columns (batch_ops reduce_specs).
class GroupBy {
   public:
    GroupBy(const DataFrame& frame, dstd::Vector<dstd::String> keys);

    const dstd::Vector<dstd::String>& keys() const noexcept { return keys_; }

    /// Any aggregates, as DataFrame::group_by takes them.
    dstd::Result<DataFrame> agg(const dstd::Vector<GroupAgg>& aggs) const;
    /// One aggregate broadcast over every eligible non-key column, each under
    /// its own name. An InvalidArgument error for an aggregate that takes a
    /// second column or a parameter (give it to agg()).
    dstd::Result<DataFrame> reduce(Agg agg) const;

    dstd::Result<DataFrame> sum() const { return reduce(Agg::Sum); }
    dstd::Result<DataFrame> mean() const { return reduce(Agg::Mean); }
    dstd::Result<DataFrame> min() const { return reduce(Agg::Min); }
    dstd::Result<DataFrame> max() const { return reduce(Agg::Max); }
    dstd::Result<DataFrame> count() const { return reduce(Agg::CountValid); }
    dstd::Result<DataFrame> var() const { return reduce(Agg::Var); }
    dstd::Result<DataFrame> std() const { return reduce(Agg::Std); }
    dstd::Result<DataFrame> skew() const { return reduce(Agg::Skew); }
    dstd::Result<DataFrame> kurt() const { return reduce(Agg::Kurt); }
    dstd::Result<DataFrame> first() const { return reduce(Agg::First); }
    dstd::Result<DataFrame> last() const { return reduce(Agg::Last); }
    /// The row count per group (pandas `size()`), as one Int64 column `size`.
    dstd::Result<DataFrame> size() const;

    /// LazyGroupBy::transform over this frame, collected: the group-wise
    /// transforms (`cumsum`, `shift`, `rank`, `head`, ...) with the same
    /// contract and refusals.
    dstd::Result<DataFrame> transform(GroupwiseOp kind, std::int64_t n = 0,
                                      RankMethod method = RankMethod::Average,
                                      bool ascending = true) const;
    dstd::Result<DataFrame> cumsum() const {
        return transform(GroupwiseOp::CumSum);
    }
    dstd::Result<DataFrame> cummax() const {
        return transform(GroupwiseOp::CumMax);
    }
    dstd::Result<DataFrame> cummin() const {
        return transform(GroupwiseOp::CumMin);
    }
    dstd::Result<DataFrame> cumcount() const {
        return transform(GroupwiseOp::CumCount);
    }
    dstd::Result<DataFrame> shift(std::int64_t periods = 1) const {
        return transform(GroupwiseOp::Shift, periods);
    }
    dstd::Result<DataFrame> diff() const {
        return transform(GroupwiseOp::Diff);
    }
    dstd::Result<DataFrame> pct_change() const {
        return transform(GroupwiseOp::PctChange);
    }
    dstd::Result<DataFrame> rank(RankMethod method = RankMethod::Average,
                                 bool ascending = true) const {
        return transform(GroupwiseOp::Rank, 0, method, ascending);
    }
    dstd::Result<DataFrame> ngroup() const {
        return transform(GroupwiseOp::NGroup);
    }
    dstd::Result<DataFrame> head(std::int64_t n = 5) const {
        return transform(GroupwiseOp::Head, n);
    }
    dstd::Result<DataFrame> tail(std::int64_t n = 5) const {
        return transform(GroupwiseOp::Tail, n);
    }
    dstd::Result<DataFrame> nth(std::int64_t n) const {
        return transform(GroupwiseOp::Nth, n);
    }
    dstd::Result<DataFrame> cumprod() const {
        return transform(GroupwiseOp::CumProd);
    }
    dstd::Result<DataFrame> ffill() const {
        return transform(GroupwiseOp::FFill);
    }
    dstd::Result<DataFrame> bfill() const {
        return transform(GroupwiseOp::BFill);
    }
    dstd::Result<DataFrame> rolling_sum(std::int64_t window) const {
        return transform(GroupwiseOp::RollingSum, window);
    }
    dstd::Result<DataFrame> rolling_mean(std::int64_t window) const {
        return transform(GroupwiseOp::RollingMean, window);
    }
    dstd::Result<DataFrame> rolling_min(std::int64_t window) const {
        return transform(GroupwiseOp::RollingMin, window);
    }
    dstd::Result<DataFrame> rolling_max(std::int64_t window) const {
        return transform(GroupwiseOp::RollingMax, window);
    }

   private:
    DataFrame frame_;
    dstd::Vector<dstd::String> keys_;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_DATAFRAME_H
