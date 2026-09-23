#ifndef DFTRACER_UTILS_DATAFRAME_LAZYFRAME_H
#define DFTRACER_UTILS_DATAFRAME_LAZYFRAME_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/op.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::dataframe {

/// Whether a morsel says anything about row order. Sequence means
/// `batch_index` is contiguous from 0 and morsels arrive in production order.
/// ByColumn means this and every later morsel from the same cursor are ordered
/// by `Morsel::ordered_column`; the claim covers the whole stream, not one
/// morsel. A producer that can promise neither reports Unordered.
enum class Ordering {
    Unordered,
    Sequence,
    ByColumn,
};

/// A chunk of columns flowing through the lazy pipeline. Carries no names - the
/// schema lives in the plan. Columns are FLAT.
struct Morsel {
    std::vector<Series> columns;
    std::int64_t rows = 0;
    /// Per-morsel schema for a streaming source whose morsels may differ in
    /// columns or types. Empty means positional alignment with the plan schema
    /// (the common case). One interned id per column, resolved via `intern`.
    std::vector<std::uint32_t> name_ids;
    std::shared_ptr<const dftracer::utils::StringIntern> intern;
    /// Name-keyed dyn value columns, carried out of band from `columns` so the
    /// positional plan schema stays fixed while the dyn set varies per morsel.
    /// `dyn_names[i]` (producer-tagged) labels `dyn_columns[i]`; a dyn group_by
    /// folds these through agg_accumulate's dyn feed. Empty for a non-dyn
    /// morsel.
    std::vector<std::string> dyn_names;
    std::vector<Series> dyn_columns;
    /// -1 when the producer keeps none.
    std::int64_t batch_index = -1;
    /// Opt in explicitly; an operation that reorders, drops, or adds rows
    /// must reset this unless it can prove the property still holds.
    Ordering ordering = Ordering::Unordered;
    /// Index into `columns`, valid only when `ordering == ByColumn`.
    std::int32_t ordered_column = -1;
    bool ordered_descending = false;
};

/// A stateful reader over one Source. next() returns the next morsel, or
/// nullopt at end; `max_rows` is a size hint. The engine pulls it async
/// (co_await), so a producer can suspend on real I/O inside next() instead of
/// blocking a worker thread.
class ReclaimRegistry;

class Cursor {
   public:
    virtual ~Cursor();
    virtual coro::CoroTask<std::optional<Morsel>> next(
        std::int64_t max_rows) = 0;
    /// Pull up to `max_rows` rows without suspending, when this cursor can
    /// answer from data it already holds. Returns false when it cannot, and
    /// the caller must await next() instead. A true return with `out` unset
    /// is end of stream, exactly as next() reports it.
    virtual bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) {
        (void)max_rows;
        (void)out;
        return false;
    }
    /// Output column names, when they are only known after producing (a
    /// data-dependent schema like pivot/to_dummies). nullopt means the plan's
    /// static schema is authoritative. Valid only after the cursor is drained.
    virtual std::optional<std::vector<std::string>> out_names() const {
        return std::nullopt;
    }

    /// Offer `predicate` (positional against this cursor's own schema, like a
    /// ScanRequest filter) as a narrowing of the work this cursor has not yet
    /// produced - a join build side or a plugin that learns a bound partway
    /// through the scan. ADVISORY: a cursor may ignore this entirely, or
    /// narrow only part of what remains; the caller must still evaluate
    /// `predicate` itself over every row next()/try_next() yields afterward,
    /// the same discipline as an unpushed (Pushed::No/Inexact) filter, so
    /// ignoring or partially applying it can only change how much work the
    /// cursor does, never the result. The return value reports whether the
    /// cursor did anything with it, for diagnostics only. Modelled on Velox's
    /// canAddDynamicFilter/addDynamicFilter.
    virtual coro::CoroTask<bool> narrow(const Expr& predicate) {
        (void)predicate;
        co_return false;
    }

    /// The bytes this cursor holds resident beyond one in-flight morsel: a
    /// build table, a spool, a cache. The driver sums it over the chain
    /// against the plan's memory budget. 0 (the default) means either
    /// nothing or unknown; a cursor that cannot measure itself is never
    /// asked to reclaim.
    virtual std::uint64_t resident_bytes() const { return 0; }
    /// Free up to `want` resident bytes however this cursor can (spill,
    /// compact, drop a cache) and return how many it freed. ADVISORY: a
    /// cursor may free less, or nothing, and the result it produces must not
    /// change. The driver calls it when the chain's resident total runs past
    /// the budget, largest holder first. Modelled on Velox's reclaim.
    virtual coro::CoroTask<std::uint64_t> reclaim(std::uint64_t want) {
        (void)want;
        co_return 0;
    }
    /// Enrol this cursor in a plan's reclaim registry: the set of stages the
    /// driver measures and asks to reclaim. The plan lowering calls it on
    /// every stage it builds, before a later stage (a plugin node included)
    /// takes ownership, so a stage is seen wherever it ends up; the cursor
    /// leaves the registry when it is destroyed. Modelled on Spark's
    /// MemoryConsumer registering with its TaskMemoryManager.
    void attach(std::shared_ptr<ReclaimRegistry> registry);

   private:
    std::shared_ptr<ReclaimRegistry> registry_;
};

/// The static, scan-free schema of a Source: one Field per column, in order.
/// A column whose type is not knowable without scanning gets a `Field` whose
/// `type.id == TypeId::Unknown`, never a guessed concrete type.
struct Schema {
    std::vector<Field> fields;
};

/// How completely a source applied a pushed-down filter, per ScanRequest
/// filter. Guides whether the engine must re-apply it over the survivors.
/// Mirrors dftu_pushed (dataframe/abi.h).
enum class Pushed {
    No = DFTU_PUSHED_NO,      ///< Not applied by the source; engine applies it.
    Inexact =
        DFTU_PUSHED_INEXACT,  ///< Pruned I/O but did not filter; re-apply.
    Exact =
        DFTU_PUSHED_EXACT,    ///< Fully applied by the source; engine drops it.
};
static_assert(static_cast<int>(Pushed::No) == DFTU_PUSHED_NO);
static_assert(static_cast<int>(Pushed::Inexact) == DFTU_PUSHED_INEXACT);
static_assert(static_cast<int>(Pushed::Exact) == DFTU_PUSHED_EXACT);

/// A pushdown request the optimizer hands a Source at scan time.
struct ScanRequest {
    /// Columns the plan needs, in the order the scan must return them. Empty
    /// means all source columns. A source that accepts a non-empty projection
    /// MUST return exactly these columns, in this order.
    std::vector<std::string> projection;
    /// Candidate predicates, each positional against the request's column set
    /// (projection when non-empty, else schema()). A source translates what it
    /// can and reports the rest No.
    std::vector<Expr> filters;
    std::int64_t limit = -1;  ///< Slice pushdown hint; -1 means no limit.
    /// The resolved LazyFrame budget (bytes); a streaming source may use it to
    /// bound its own in-flight buffering. Most sources ignore it.
    std::uint64_t memory_budget = 0;
};

/// The result of Source::scan: a fresh async Cursor plus, per ScanRequest
/// filter, how completely the source applied it.
struct ScanResult {
    std::unique_ptr<Cursor> cursor;
    std::vector<Pushed> filters;
};

/// A pushdown-aware data source for a lazy query. Immutable: schema() reports
/// the columns without scanning and scan() hands out a fresh Cursor honoring
/// the pushed projection/filters, so one Source can back many collect()s.
/// Implement these two to plug any producer (a file, another engine, a trace
/// scan) into LazyFrame.
class Source {
   public:
    virtual ~Source() = default;
    /// The column names and types, without a scan (index/catalog/footer).
    virtual Schema schema() const = 0;
    /// Open a Cursor honoring `req`. When req.projection is non-empty the
    /// cursor's morsels carry exactly those columns in that order; the returned
    /// ScanResult.filters reports, per req.filter, how completely it was
    /// applied.
    virtual ScanResult scan(const ScanRequest& req) const = 0;
    /// The resident frame when this source is already in memory, else nullptr
    /// (rows produced only by streaming). collect() runs a resident source
    /// whole-column, matching the eager path instead of paying the morsel tax.
    virtual const DataFrame* as_frame() const { return nullptr; }

    /// Convenience: the schema's column names. Non-virtual; a caller that only
    /// needs names reads this instead of building a full scan.
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        Schema s = schema();
        out.reserve(s.fields.size());
        for (const Field& f : s.fields) out.push_back(f.name);
        return out;
    }
};

/// A Source over an already-materialized in-memory frame.
class InMemorySource : public Source {
   public:
    explicit InMemorySource(DataFrame frame);
    Schema schema() const override;
    ScanResult scan(const ScanRequest& req) const override;
    const DataFrame* as_frame() const override { return frame_.get(); }

   private:
    std::shared_ptr<const DataFrame> frame_;
};

/// A deferred query over a Source. Builder methods record ops and return a new
/// LazyFrame; nothing runs until collect(). A filter/with_column expr's col(i)
/// refers to the i-th column of the frame at that point.
class LazyOp;
class LazyGroupBy;

class LazyFrame {
   public:
    static LazyFrame scan(std::shared_ptr<const Source> source);

    LazyFrame select(std::vector<std::string> names) const;
    LazyFrame filter(Expr predicate) const;
    LazyFrame with_column(std::string name, Expr expr) const;
    LazyFrame rename(std::vector<std::string> names) const;
    LazyFrame slice(std::int64_t offset, std::int64_t len) const;
    LazyFrame head(std::int64_t n) const;
    LazyFrame tail(std::int64_t n) const;
    LazyFrame drop_nulls() const;
    LazyFrame fill_null(Scalar value) const;
    /// Keeps the rows at `indices` (arbitrary order, repeats allowed), as
    /// DataFrame::take. Not streaming: the whole frame must be resident before
    /// the indices can be applied.
    LazyFrame take(std::vector<std::int64_t> indices) const;
    /// Keeps rows where the precomputed `mask` is true, positionally aligned to
    /// this frame's rows. Streams: each morsel consumes the matching mask
    /// slice. Named apart from filter(Expr) - a mask column is data, not a
    /// predicate to compile - so the two never collide at a call site.
    LazyFrame filter_mask(Series mask) const;
    /// Reverses row order, as DataFrame::reverse. Not streaming: every row
    /// must be resident before it can be reordered.
    LazyFrame reverse() const;
    /// Fill nulls with a natural C++ value; converts to each column's type.
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    LazyFrame fill_null(T value) const {
        return fill_null(to_scalar(value));
    }
    LazyFrame with_row_index(std::string name) const;
    /// A one-row frame of each column's null count.
    LazyFrame null_count() const;
    /// Expand a List `column`: each element becomes its own row.
    LazyFrame explode(std::string column) const;
    /// UNNEST a List `column` per morsel, as DataFrame::unnest: an empty or
    /// null list drops the row unless `keep_empty`, and a List<Struct>
    /// flattens into one column per field. schema() names the fields when the
    /// column's type is known at build time; otherwise it is empty until
    /// collect(). Throws std::out_of_range for an absent column and
    /// std::invalid_argument for a non-List one, when the schema can tell.
    LazyFrame unnest(std::string column, bool keep_empty = false) const;
    /// Reshape wide -> long: keep `id_vars`, stack `value_vars` into a
    /// variable/value column pair.
    LazyFrame unpivot(std::vector<std::string> id_vars,
                      std::vector<std::string> value_vars) const;
    /// The `k` rows with the largest (or smallest) `name` values.
    LazyFrame topk(std::string name, std::int64_t k, bool largest = true) const;
    /// Group by `key` and compute each aggregate (streaming: one partial state,
    /// bounded by the group count).
    LazyFrame group_by(std::string key, std::vector<GroupAgg> aggs) const;
    /// Group by N key columns (a composite key: hashed and compared
    /// column-by-column, each keeping its own type). Streaming, as the
    /// single-key overload. `dyn` (optional) enables the name-keyed dyn
    /// side-table: each morsel's dyn columns (its own out-of-band dyn set, or a
    /// resident source's columns whose name starts with `dyn_prefix`) are
    /// folded through agg_accumulate's dyn feed, with `dyn_prefix` stripped
    /// from each name. The dyn output columns are appended after the fixed
    /// aggregates.
    LazyFrame group_by(std::vector<std::string> keys,
                       std::vector<GroupAgg> aggs,
                       std::vector<AggDynSpec> dyn = {},
                       std::string dyn_prefix = {}) const;
    /// The specs that broadcast `agg` over this plan's schema as it stands
    /// (batch_ops reduce_specs over the typed schema; a column of unknown
    /// type is included and refused at collect if it turns out non-numeric).
    /// Throws std::invalid_argument for an `agg` that does not broadcast,
    /// and std::out_of_range for a key the schema lacks.
    std::vector<GroupAgg> reduce_specs(
        Agg agg, const std::vector<std::string>& keys = {}) const;
    /// DataFrame::reduce as a plan step: the streaming one-group group_by
    /// with `agg` broadcast over the eligible columns. The grouped form is
    /// group_by(keys).reduce(agg).
    LazyFrame reduce(Agg agg) const;
    /// The two-step form: the keys now, the aggregates on the returned
    /// LazyGroupBy (`lf.group_by({"k"}).sum()`, `.agg(specs)`), each a
    /// streaming group_by step. Empty keys make the whole plan one group.
    LazyGroupBy group_by(std::vector<std::string> keys) const;
    /// Group by an expression key and compute each expression aggregate.
    /// Desugars to with_column + the string group_by: a bare column-ref key or
    /// aggregate value is used directly, a computed one is materialized into a
    /// hidden temp column first. Fully streaming, same as the string overload.
    LazyFrame group_by(Expr key, std::vector<AggExprSpec> aggs) const;
    /// Group by N expression keys; each desugars like the single-key overload.
    LazyFrame group_by(std::vector<Expr> keys,
                       std::vector<AggExprSpec> aggs) const;
    /// Sort by `name`. External merge sort: spills sorted runs past the memory
    /// budget and k-way merges them, so peak memory stays bounded.
    LazyFrame sort_by(std::string name, bool descending = false) const;
    /// Stable lexicographic sort by several key columns (nulls last), as
    /// DataFrame::sort_by_multi. Not streaming: needs every row to compare
    /// across the whole frame.
    LazyFrame sort_by_multi(std::vector<std::string> by,
                            bool descending = false) const;
    /// Per-column direction form: `descending[i]` applies to `by[i]`.
    LazyFrame sort_by_multi(std::vector<std::string> by,
                            std::vector<bool> descending) const;
    /// Distinct rows, first occurrence, in original order, keyed on every
    /// column or on `subset` (empty = all). Streams input and output; holds
    /// only the distinct key set. Throws std::out_of_range for a subset name
    /// the schema lacks, when the schema is known.
    LazyFrame unique(std::vector<std::string> subset = {}) const;
    /// Alias of unique().
    LazyFrame drop_duplicates(std::vector<std::string> subset = {}) const;
    /// A deterministic n-row sample (mix64 min-hash). Streaming: bounded to n
    /// rows regardless of input size.
    LazyFrame sample(std::int64_t n, std::uint64_t seed = 0) const;
    /// One Bool column: true where the whole row is duplicated. Two-pass
    /// (count, then per-row mask in input order); state is the count map.
    LazyFrame is_duplicated() const;
    /// One Bool column: true where the whole row is unique. Two-pass, as
    /// is_duplicated.
    LazyFrame is_unique() const;
    /// Tumbling/sliding time-window aggregation over an ascending Int64 time
    /// column. Streaming: holds one agg state per open window (bounded by the
    /// window count, not the input). Requires ascending time.
    LazyFrame group_by_dynamic(std::string time_col, std::int64_t every,
                               std::int64_t period, std::vector<GroupAgg> aggs,
                               std::int64_t origin = 0,
                               bool origin_min = false) const;
    /// unpivot alias.
    LazyFrame melt(std::vector<std::string> id_vars,
                   std::vector<std::string> value_vars) const;
    /// Hash join with `other` on `left_on[i]` = `right_on[i]`, as
    /// DataFrame::join. `other` is collected in full when this plan runs (the
    /// build side, bounded by the right row count); this plan streams through
    /// it morsel by morsel, so Inner / Left / Semi / Anti / Cross hold no
    /// left state and Right / Outer add one match bit per right row. An
    /// optimizer barrier: no filter or projection moves across it. An absent
    /// key or a key type mismatch is reported at collect().
    LazyFrame join(LazyFrame other, std::vector<std::string> left_on,
                   std::vector<std::string> right_on,
                   JoinHow how = JoinHow::Inner,
                   std::string suffix = "_right") const;
    /// Join on the same-named key columns `on`.
    LazyFrame join(LazyFrame other, std::vector<std::string> on,
                   JoinHow how = JoinHow::Inner,
                   std::string suffix = "_right") const;
    /// Vertical concatenation: every row of this plan, then every row of
    /// `other`. Both plans must have the same column names in the same order
    /// with the same types where known; a mismatch throws
    /// std::invalid_argument here. Streams both sides in turn with no state.
    /// An optimizer barrier: no filter or projection moves across it.
    LazyFrame concat(LazyFrame other) const;
    /// DataFrame::compare_agg over the two collected plans: keys, `l_`/`r_`
    /// per metric, then `delta_`/`pct_` per numeric metric. Output names are
    /// data-dependent, so schema() is empty until collect(). Runs through
    /// frame_op("dftu.frame.compare_agg").
    LazyFrame compare_agg(LazyFrame variant, std::int64_t n_key) const;
    /// Any registry table -> table op (a `dftu.frame.*` row, or one another
    /// library registered, such as `dftu.frame.window`) as a plan step: this
    /// plan is collected and passed as the op's first FRAME operand, each
    /// further FRAME operand takes the next plan of `others` (collected when
    /// the op runs), every other operand is deep-copied from `args` now so
    /// the plan owns it. A pipeline breaker and an optimizer barrier. The
    /// output names are `out_names` when the caller can state them from the
    /// op's contract (a window keeps its input and appends its specs), else
    /// data-dependent (schema() empty until collect()). Throws
    /// std::invalid_argument if no such op is registered, it is not a
    /// table -> table op, an operand has no owned form (EXPR / QUERY / LAZY)
    /// or `others` does not match the op's frame operands.
    LazyFrame frame_op(std::string name, OpArgs args,
                       std::vector<LazyFrame> others = {},
                       std::vector<std::string> out_names = {}) const;
    /// Reshape long -> wide. Two-pass (distinct index+on values, then cell
    /// aggregation via the agg IR); state is bounded by the output (index rows
    /// x on-values). Output columns are data-dependent (one per distinct `on`
    /// value), so schema() is empty until collect().
    LazyFrame pivot(std::string index, std::string on, std::string values,
                    std::string agg = "first") const;
    /// One-hot encode `column`. Two-pass (distinct values, then stream-encode);
    /// state is bounded by the value cardinality. Output columns are
    /// data-dependent, so schema() is empty until collect().
    LazyFrame to_dummies(std::string column) const;
    /// Per-column summary statistics (count/null_count/mean/std/min/max).
    /// Streaming, constant state per column; output columns are data-dependent
    /// (one per numeric input column), so schema() is empty until collect().
    LazyFrame describe() const;

    /// Out-of-core budget for the pipeline breakers (sort/unique/group_by):
    /// when a sink's in-memory state grows past this many bytes it spills to a
    /// sorted temp run, k-way merged at the end, so peak memory stays bounded.
    /// 0 (the default) means "auto": ~1/3 of available memory. Pass
    /// NO_SPILL_BUDGET to disable spilling. Same knob and policy as View.
    LazyFrame memory_budget(std::uint64_t bytes) const;
    /// Explicitly set the budget to ~1/3 of available memory (same as the
    /// default); sugar for readers who want spilling stated at the call site.
    LazyFrame auto_spill() const;

    /// Append a step running the plan node registered under `name`
    /// (dftu_node_register) as the next stage in the pull chain
    /// lower_cursor_chain builds - a step the engine executes when this plan
    /// is driven, not eagerly here. Resolves and captures the node's vt/self
    /// now, so a later dftu_node_unregister of the same name is the same
    /// dangling-registration hazard dftu_provider_unregister documents for a
    /// captured provider, not a lookup that could fail at drive time.
    /// Throws std::invalid_argument immediately if no node is registered
    /// under `name`. The node is an optimization barrier: no filter or
    /// projection is ever pushed through it, and the engine never reorders
    /// around it. `args` is copied by value; a pointer-bearing operand it
    /// carries must outlive every future execution of the returned
    /// LazyFrame, not merely this call.
    LazyFrame op(std::string name, OpArgs args) const;

    /// Output column names without running the query. Empty for a plan ending
    /// in a data-dependent op (pivot/to_dummies/describe).
    std::vector<std::string> schema() const;

    /// The typed output schema without running the query: walks the plan from
    /// the source's Schema, transforming it per op the same way schema()
    /// transforms names. Empty for a data-dependent op (pivot/to_dummies/
    /// describe), matching schema(). A field whose type an op cannot
    /// determine statically is TypeId::Unknown, never a guess.
    Schema output_schema() const;

    /// The optimized plan as text (source then one op per line), for
    /// introspection and tests.
    std::string explain() const;

    /// Pull-based chunk generator: the streaming terminal. Each yielded
    /// DataFrame is standalone, carrying its own schema. One-shot: re-run
    /// from the LazyFrame to restart. `morsel_rows` is the scan chunk size;
    /// <= 0 means auto. collect() drains this.
    coro::AsyncGenerator<DataFrame> stream(std::int64_t morsel_rows = 0) const;

    /// The plan's cursor chain, undrained, so a caller can read each morsel's
    /// ordering before stream() materializes it away. One-shot, as stream().
    std::unique_ptr<Cursor> open_cursor() const;

    /// Run the pipeline and materialize the surviving rows. `morsel_rows` is
    /// the scan chunk size; <= 0 (the default) means auto; one pass over a
    /// resident source, the bounded streaming default otherwise.
    coro::CoroTask<DataFrame> collect(std::int64_t morsel_rows = 0) const;

    /// Group `keys` with `aggs` and return the mergeable partial instead of a
    /// finalized frame: streams this pipeline (no group op appended) and folds
    /// every morsel into one AggState. The caller finalizes (agg_finalize),
    /// coarsens (agg_regroup), or persists it. State is bounded by the distinct
    /// group count, not the input; used by the rollup materialize path.
    coro::CoroTask<AggStatePtr> collect_group_state(
        std::vector<std::string> keys, std::vector<GroupAgg> aggs,
        std::vector<AggDynSpec> dyn = {}, std::string dyn_prefix = {},
        std::int64_t morsel_rows = 0) const;

   private:
    LazyFrame(std::shared_ptr<const Source> source,
              std::vector<std::shared_ptr<const LazyOp>> ops,
              std::uint64_t memory_budget = 0)
        : source_(std::move(source)),
          ops_(std::move(ops)),
          memory_budget_(memory_budget) {}
    // Clone with a new op list, preserving the source and budget.
    LazyFrame with_ops(std::vector<std::shared_ptr<const LazyOp>> ops) const {
        return LazyFrame(source_, std::move(ops), memory_budget_);
    }
    // The ops over a resident frame, whole-column: an op with an eager form
    // runs it, any other runs its own cursor over the frame as one morsel.
    static coro::CoroTask<DataFrame> run_ops_in_memory(
        DataFrame df, std::vector<std::shared_ptr<const LazyOp>> ops);
    std::shared_ptr<const Source> source_;
    std::vector<std::shared_ptr<const LazyOp>> ops_;
    std::uint64_t memory_budget_ = 0;
};

/// The two-step lazy group-by: a plan and its key columns, with the
/// aggregates chosen on it. Every method appends one streaming group_by step
/// (the same GroupByOp LazyFrame::group_by(keys, specs) records); the family
/// below broadcasts one aggregate over the eligible non-key columns.
class LazyGroupBy {
   public:
    LazyGroupBy(LazyFrame plan, std::vector<std::string> keys);

    const std::vector<std::string>& keys() const noexcept { return keys_; }

    LazyFrame agg(std::vector<GroupAgg> aggs) const;
    LazyFrame reduce(Agg agg) const;

    LazyFrame sum() const { return reduce(Agg::Sum); }
    LazyFrame mean() const { return reduce(Agg::Mean); }
    LazyFrame min() const { return reduce(Agg::Min); }
    LazyFrame max() const { return reduce(Agg::Max); }
    LazyFrame count() const { return reduce(Agg::CountValid); }
    LazyFrame var() const { return reduce(Agg::Var); }
    LazyFrame std() const { return reduce(Agg::Std); }
    LazyFrame skew() const { return reduce(Agg::Skew); }
    LazyFrame kurt() const { return reduce(Agg::Kurt); }
    LazyFrame first() const { return reduce(Agg::First); }
    LazyFrame last() const { return reduce(Agg::Last); }
    LazyFrame size() const;

    /// The group-wise transforms (pandas `groupby(k).cumsum()` and kin): one
    /// value per input row, rows in input order. Each is the window kernel
    /// (`dftu.frame.window`, registered by the utilities library) PARTITION
    /// BY the keys ORDER BY a row index the plan prepends, then the sort back
    /// and the projection. CumSum / CumProd (Float64) / CumMax / CumMin /
    /// Diff / PctChange / Rank take every numeric non-key column, Shift every
    /// non-key column, each under its own name; CumCount / NGroup are one Int64
    /// column of that name; Head / Tail / Nth keep the input columns of the
    /// selected rows; FFill / BFill fill each non-key column's nulls from the
    /// nearest present value before / after it within the group; RollingSum /
    /// Mean / Min / Max reduce the trailing `n` rows of each numeric non-key
    /// column within the group, null until the window holds `n` present values
    /// (the pandas default `min_periods`). A null value stays null. `n` is the
    /// Shift periods (negative looks ahead), the Head / Tail count, the Nth
    /// position (0-based, negative from the end) or the rolling window;
    /// `method` / `ascending` apply to Rank (Float64, as pandas; Ordinal is
    /// pandas' `first`). Throws std::invalid_argument when
    /// no column is eligible, NGroup has no keys or the window op is not
    /// registered; std::out_of_range for an absent key.
    LazyFrame transform(GroupwiseOp kind, std::int64_t n = 0,
                        RankMethod method = RankMethod::Average,
                        bool ascending = true) const;
    LazyFrame cumsum() const { return transform(GroupwiseOp::CumSum); }
    LazyFrame cummax() const { return transform(GroupwiseOp::CumMax); }
    LazyFrame cummin() const { return transform(GroupwiseOp::CumMin); }
    LazyFrame cumcount() const { return transform(GroupwiseOp::CumCount); }
    LazyFrame shift(std::int64_t periods = 1) const {
        return transform(GroupwiseOp::Shift, periods);
    }
    LazyFrame diff() const { return transform(GroupwiseOp::Diff); }
    LazyFrame pct_change() const { return transform(GroupwiseOp::PctChange); }
    LazyFrame rank(RankMethod method = RankMethod::Average,
                   bool ascending = true) const {
        return transform(GroupwiseOp::Rank, 0, method, ascending);
    }
    LazyFrame ngroup() const { return transform(GroupwiseOp::NGroup); }
    LazyFrame head(std::int64_t n = 5) const {
        return transform(GroupwiseOp::Head, n);
    }
    LazyFrame tail(std::int64_t n = 5) const {
        return transform(GroupwiseOp::Tail, n);
    }
    LazyFrame nth(std::int64_t n) const {
        return transform(GroupwiseOp::Nth, n);
    }
    LazyFrame cumprod() const { return transform(GroupwiseOp::CumProd); }
    LazyFrame ffill() const { return transform(GroupwiseOp::FFill); }
    LazyFrame bfill() const { return transform(GroupwiseOp::BFill); }
    LazyFrame rolling_sum(std::int64_t window) const {
        return transform(GroupwiseOp::RollingSum, window);
    }
    LazyFrame rolling_mean(std::int64_t window) const {
        return transform(GroupwiseOp::RollingMean, window);
    }
    LazyFrame rolling_min(std::int64_t window) const {
        return transform(GroupwiseOp::RollingMin, window);
    }
    LazyFrame rolling_max(std::int64_t window) const {
        return transform(GroupwiseOp::RollingMax, window);
    }

   private:
    LazyFrame plan_;
    std::vector<std::string> keys_;
};

/// Free-function form of DataFrame::lazy(), for `lazy(df)` call sites.
LazyFrame lazy(DataFrame frame);

/// Lower name-based GroupAggs to index-based AggSpecs plus the deduped list of
/// value column names they reference (Count references none); the i-th spec's
/// value_col/by_col index into `value_names`. The gagg lowering that
/// collect_group_state and any external chunk driver share, so a Fold feeding
/// its own chunks accumulates into a byte-identical AggState.
struct LoweredGroupAggs {
    std::vector<AggSpec> specs;
    std::vector<std::string> value_names;
};
LoweredGroupAggs lower_group_aggs(const std::vector<GroupAgg>& aggs);

/// Accumulate one already-built chunk `frame` into `state`: resolve `keys` and
/// `value_names` (from lower_group_aggs) by name against `frame`, feeding every
/// column whose name starts with `dyn_prefix` as a dyn input (prefix stripped).
/// The per-morsel step collect_group_state runs, exposed for a non-LazyFrame
/// chunk driver (a trace Fold accumulating over a shared scan).
void agg_accumulate_chunk(AggState& state, const DataFrame& frame,
                          const std::vector<std::string>& keys,
                          const std::vector<std::string>& value_names,
                          const std::string& dyn_prefix);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_LAZYFRAME_H
