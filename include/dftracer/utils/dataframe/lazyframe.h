// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_LAZYFRAME_H
#define DFTRACER_UTILS_DATAFRAME_LAZYFRAME_H

#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/tuple.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace dftracer::utils::dataframe {

/// Whether a morsel says anything about row order. Sequence means
/// `batch_index` is contiguous from 0 and morsels arrive in production order.
/// ByColumn means this and every later morsel from the same cursor are ordered
/// by `Morsel::ordered_column`; the claim covers the whole stream, not one
/// morsel. A producer that can promise neither reports Unordered.
enum class Ordering : std::uint8_t {
    Unordered,
    Sequence,
    ByColumn,
};

/// The out-of-band part of a morsel from a streaming source whose morsels may
/// differ in columns or types.
struct MorselDyn {
    /// One interned id per column of Morsel::columns, resolved via `intern`;
    /// empty means positional alignment with the plan schema.
    dstd::Vector<std::uint32_t> name_ids;
    dstd::SharedPtr<const dftracer::utils::dstd::StringIntern> intern;
};

/// A chunk of columns flowing through the lazy pipeline. Carries no names - the
/// schema lives in the plan. Columns are FLAT.
struct Morsel {
    dstd::Vector<Series> columns;
    std::int64_t rows = 0;
    /// -1 when the producer keeps none.
    std::int64_t batch_index = -1;
    /// Null for a positional morsel (the common case).
    dstd::UniquePtr<MorselDyn> dyn;
    /// Opt in explicitly; an operation that reorders, drops, or adds rows
    /// must reset this unless it can prove the property still holds.
    Ordering ordering = Ordering::Unordered;
    /// Valid only when `ordering == ByColumn`: an index into `columns`.
    bool ordered_descending = false;
    std::int32_t ordered_column = -1;

    /// The per-column names, empty for a positional morsel.
    const dstd::Vector<std::uint32_t>& name_ids() const {
        static const dstd::Vector<std::uint32_t> NONE;
        return dyn ? dyn->name_ids : NONE;
    }
    /// The out-of-band part, created on first use.
    MorselDyn& dyn_state() {
        if (!dyn) dyn = dstd::make_unique<MorselDyn>();
        return *dyn;
    }
};

/// What reading a @ref Cursor gives: the next morsel, nullopt at the end of
/// the stream, or the error that ended the read. After an error the cursor
/// gives nothing more.
using MorselResult = dstd::Result<dstd::Optional<Morsel>>;

/// A stateful reader over one Source. next() returns the next morsel, or
/// nullopt at end, or the error that ended the read; `max_rows` is a size
/// hint. The engine pulls it async (co_await), so a producer can suspend on
/// real I/O inside next() instead of blocking a worker thread. A cursor that
/// a source hands out may still throw from next(), narrow() or reclaim(): the
/// engine awaits those through @ref coro::settle and turns an exception into
/// an error result (a failed narrow() or reclaim() counts as nothing done).
/// try_next(), out_names() and resident_bytes() must not throw.
class Cursor {
   public:
    virtual ~Cursor();
    virtual coro::CoroTask<MorselResult> next(std::int64_t max_rows) = 0;
    /// Pull up to `max_rows` rows without suspending, when this cursor can
    /// answer from data it already holds. Returns false when it cannot, and
    /// the caller must await next() instead. A true return with `out` holding
    /// no morsel is end of stream, and one holding an error is a failed read,
    /// exactly as next() reports them. Must not throw.
    virtual bool try_next(std::int64_t, MorselResult&) { return false; }
    /// Output column names, when they are only known after producing (a
    /// data-dependent schema like pivot/to_dummies). nullopt means the plan's
    /// static schema is authoritative. Valid only after the cursor is drained.
    virtual dstd::Optional<dstd::Vector<dstd::String>> out_names() const {
        return dstd::nullopt;
    }

    /// Offer `predicate` (positional against this cursor's own schema, like a
    /// @ref ScanRequest filter) as a narrowing of the work this cursor has not
    /// yet produced - a join build side or a plugin that learns a bound partway
    /// through the scan. ADVISORY: a cursor may ignore this entirely, or
    /// narrow only part of what remains; the caller must still evaluate
    /// `predicate` itself over every row next()/try_next() yields afterward,
    /// the same discipline as an unpushed (Pushed::No/Inexact) filter, so
    /// ignoring or partially applying it can only change how much work the
    /// cursor does, never the result. The return value reports whether the
    /// cursor did anything with it, for diagnostics only. Modelled on Velox's
    /// canAddDynamicFilter/addDynamicFilter.
    virtual coro::CoroTask<bool> narrow(const Expr&) { co_return false; }

    /// The bytes this cursor holds resident beyond one in-flight morsel: a
    /// build table, a spool, a cache. 0 (the default) means nothing or unknown.
    virtual std::uint64_t resident_bytes() const { return 0; }
    /// Free up to `want` resident bytes however this cursor can (spill,
    /// compact, drop a cache) and return how many it freed. ADVISORY: a
    /// cursor may free less, or nothing, and the result it produces must not
    /// change. A plugin node may call it on its input through the cursor ABI.
    virtual coro::CoroTask<std::uint64_t> reclaim(std::uint64_t) {
        co_return 0;
    }
};

/// The static, scan-free schema of a Source: one Field per column, in order.
/// A column whose type is not knowable without scanning gets a `Field` whose
/// `type.id == TypeId::Unknown`, never a guessed concrete type.
struct Schema {
    dstd::Vector<Field> fields;
};

/// How completely a source applied a pushed-down filter, per @ref ScanRequest
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
    dstd::Vector<dstd::String> projection;
    /// Candidate predicates, each positional against the request's column set
    /// (projection when non-empty, else schema()). A source translates what it
    /// can and reports the rest No.
    dstd::Vector<Expr> filters;
    std::int64_t limit = -1;  ///< Slice pushdown hint; -1 means no limit.
    /// The resolved @ref LazyFrame budget (bytes); a streaming source may use
    /// it to bound its own in-flight buffering. Most sources ignore it.
    std::uint64_t memory_budget = 0;
};

/// What Source::scan gives: a fresh async Cursor plus, per @ref ScanRequest
/// filter, how completely the source applied it.
struct ScanResult {
    dstd::UniquePtr<Cursor> cursor;
    dstd::Vector<Pushed> filters;
};

class Source;

/// How a source took an op offered by a planning hook (Source::apply_*).
/// Mirrors dftu_apply_status (dataframe/abi.h).
enum class ApplyStatus {
    Exact,    ///< Fully applied; the planner removes the op.
    Inexact,  ///< Rows narrowed only; the planner keeps the op above.
};

/// A planning hook's accepted result: a new immutable source that carries the
/// op. Its schema() is the output after the op.
struct SourceApplication {
    dstd::SharedPtr<const Source> source;
    ApplyStatus status = ApplyStatus::Exact;
};

/// One output column of a projection offered to Source::apply_projection.
/// `expr` is positional against the offering source's schema().
struct NamedExpr {
    dstd::String name;
    Expr expr;
};

/// One aggregate of an @ref AggregateSpec. `input` and `by` are positional
/// against the offering source's schema(). `input` is unset for count; `by` is
/// set only for the ops that read a second column (agg_uses_by_col). `param` is
/// the quantile or k where the op takes one.
struct AggregateExpr {
    Agg op = Agg::Count;
    Expr input;
    Expr by;
    double param = 0.0;
    dstd::String out;
};

/// A group-by offered to Source::apply_aggregation. The output is one column
/// per key, named by its `name`, in order, then one column per agg named by
/// its `out`.
struct AggregateSpec {
    dstd::Vector<NamedExpr> keys;
    dstd::Vector<AggregateExpr> aggs;
};

/// A lexicographic ordering: `by[i]` sorts descending when `descending[i]`.
struct SortSpec {
    dstd::Vector<dstd::String> by;
    dstd::Vector<bool> descending;
};

/// A join offered to Source::apply_join. `other` is the right side with every
/// op it had already absorbed, so nothing remains to run above it. The output
/// is what LazyFrame::join makes for the same arguments.
struct JoinSpec {
    dstd::SharedPtr<const Source> other;
    dstd::Vector<dstd::String> left_on;
    dstd::Vector<dstd::String> right_on;
    JoinHow how = JoinHow::Inner;
    dstd::String suffix;
};

/// A pushdown-aware data source for a lazy query. Immutable: schema() reports
/// the columns without scanning and scan() hands out a fresh Cursor honoring
/// the pushed projection/filters, so one Source can back many collect()s.
/// Implement these two to plug any producer (a file, another engine, a trace
/// scan) into @ref LazyFrame.
///
/// The apply_* planning hooks let a source absorb the op directly above it at
/// plan time, before scan(). Each returns dstd::nullopt for "unsupported or no
/// change", which is the default. An accepted op must yield a new source
/// handle, and offering the same op to that handle again must return
/// dstd::nullopt. A schema-changing op (projection, aggregation, join) may only
/// be accepted as Exact.
class Source {
   public:
    virtual ~Source() = default;
    /// The column names and types, without a scan (index/catalog/footer).
    virtual Schema schema() const = 0;
    /// Open a Cursor honoring `req`. When req.projection is non-empty the
    /// cursor's morsels carry exactly those columns in that order; the returned
    /// ScanResult.filters reports, per req.filter, how completely it was
    /// applied. A scan that cannot open returns the error; it must not throw,
    /// so a source built with exceptions catches at its own edge.
    virtual dstd::Result<ScanResult> scan(const ScanRequest& req) const = 0;
    /// The resident frame when this source is already in memory, else nullptr
    /// (rows produced only by streaming). collect() runs a resident source
    /// whole-column, matching the eager path instead of paying the morsel tax.
    virtual const DataFrame* as_frame() const { return nullptr; }
    /// Whether scan() may return morsels whose name_ids carry columns that
    /// schema() does not declare. A plan over such a source keeps them through
    /// every op that keeps rows and returns them, by name, after the declared
    /// columns; it refuses an op that has no rule for them.
    virtual bool undeclared_columns() const { return false; }

    virtual dstd::Optional<SourceApplication> apply_filter(const Expr&) const {
        return dstd::nullopt;
    }
    virtual dstd::Optional<SourceApplication> apply_projection(
        const dstd::Vector<NamedExpr>&) const {
        return dstd::nullopt;
    }
    virtual dstd::Optional<SourceApplication> apply_aggregation(
        const AggregateSpec&) const {
        return dstd::nullopt;
    }
    /// Distinct rows by `subset` (empty = every column), keeping the first
    /// occurrence in source order. The output keeps every column and its
    /// schema. A source that accepts it must produce the first-occurrence
    /// order itself; the planner removes the UniqueOp.
    virtual dstd::Optional<SourceApplication> apply_distinct(
        const dstd::Vector<dstd::String>&) const {
        return dstd::nullopt;
    }
    virtual dstd::Optional<SourceApplication> apply_sort(
        const SortSpec&) const {
        return dstd::nullopt;
    }
    /// The first `k` rows of apply_sort(spec). Exact means no more than `k`
    /// rows come back, already in order.
    virtual dstd::Optional<SourceApplication> apply_topn(const SortSpec&,
                                                         std::int64_t) const {
        return dstd::nullopt;
    }
    /// Rows [offset, offset + n) in source order.
    virtual dstd::Optional<SourceApplication> apply_limit(std::int64_t,
                                                          std::int64_t) const {
        return dstd::nullopt;
    }
    /// The last `n` rows in source order.
    virtual dstd::Optional<SourceApplication> apply_tail(std::int64_t) const {
        return dstd::nullopt;
    }
    virtual dstd::Optional<SourceApplication> apply_join(
        const JoinSpec&) const {
        return dstd::nullopt;
    }

    /// Sources with equal non-empty keys can run as one batch through
    /// collect_batch(), so collect_all() reads their data once. The default,
    /// dstd::nullopt, never batches.
    virtual dstd::Optional<dstd::String> batch_key() const {
        return dstd::nullopt;
    }
    /// One frame per member, in order, each what collecting that member alone
    /// would return. Called on members.front(); every member shares its
    /// batch_key(). The default is fatal: a source that returns a batch_key()
    /// must override it. An override may throw, as next() of a cursor may: the
    /// engine awaits it through @ref coro::settle and turns an exception into
    /// an error result.
    virtual coro::CoroTask<dstd::Vector<DataFrame>> collect_batch(
        dstd::Vector<dstd::SharedPtr<const Source>> members) const;
    /// One cursor per member, in order, over one shared read that runs in the
    /// background, each yielding what scanning that member alone would. Each
    /// cursor's buffer holds at most `memory_budget` bytes and throttles the
    /// shared read, so the caller must drain every cursor concurrently.
    /// Called on members.front() for members sharing its batch_key();
    /// dstd::nullopt (the default) means the source only batches through
    /// collect_batch(). An error result fails the collect; it must not throw.
    virtual dstd::Result<dstd::Optional<dstd::Vector<dstd::UniquePtr<Cursor>>>>
    open_batch(dstd::Vector<dstd::SharedPtr<const Source>>,
               std::uint64_t) const {
        return dstd::Optional<dstd::Vector<dstd::UniquePtr<Cursor>>>{};
    }

    /// Whether open_batch() can feed this member. A member that cannot (it
    /// needs its rows in scan order, say) is read through collect_batch()
    /// with the others like it, so it does not hold the streaming members
    /// back. The default is true.
    virtual bool streams_in_batch() const { return true; }

    /// Convenience: the schema's column names. Non-virtual; a caller that only
    /// needs names reads this instead of building a full scan.
    dstd::Vector<dstd::String> names() const {
        dstd::Vector<dstd::String> out;
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
    dstd::Result<ScanResult> scan(const ScanRequest& req) const override;
    const DataFrame* as_frame() const override { return frame_.get(); }

   private:
    dstd::SharedPtr<const DataFrame> frame_;
};

/// A deferred query over a Source. Builder methods record ops and return a new
/// @ref LazyFrame; nothing runs until collect(). A filter/with_column expr's
/// col(i) refers to the i-th column of the frame at that point.
class LazyOp;
class LazyGroupBy;
namespace detail {
struct PlanAccess;
}

class LazyFrame {
   public:
    static LazyFrame scan(dstd::SharedPtr<const Source> source);

    LazyFrame select(dstd::Vector<dstd::String> names) const;
    LazyFrame filter(Expr predicate) const;
    LazyFrame with_column(dstd::String name, Expr expr) const;
    LazyFrame rename(dstd::Vector<dstd::String> names) const;
    /// Removes the named columns, including those a streaming source adds per
    /// morsel. A name no column carries is ignored.
    LazyFrame drop(dstd::Vector<dstd::String> names) const;
    /// Renames each `from` column to the `to` name at the same index, static or
    /// per-morsel. A name no column carries is ignored. An InvalidArgument
    /// error, kept as @ref status() and returned by every terminal, when the
    /// lengths differ or two columns end up with one name (at plan time for
    /// static columns, per morsel for added ones).
    LazyFrame rename_columns(dstd::Vector<dstd::String> from,
                             dstd::Vector<dstd::String> to) const;
    LazyFrame slice(std::int64_t offset, std::int64_t len) const;
    LazyFrame head(std::int64_t n) const;
    LazyFrame tail(std::int64_t n) const;
    LazyFrame drop_nulls() const;
    LazyFrame fill_null(Scalar value) const;
    /// Keeps the rows at `indices` (arbitrary order, repeats allowed), as
    /// DataFrame::take. Not streaming: the whole frame must be resident before
    /// the indices can be applied.
    LazyFrame take(dstd::Vector<std::int64_t> indices) const;
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
    LazyFrame with_row_index(dstd::String name) const;
    /// A one-row frame of each column's null count.
    LazyFrame null_count() const;
    /// Expand a List `column`: each element becomes its own row.
    LazyFrame explode(dstd::String column) const;
    /// UNNEST a List `column` per morsel, as DataFrame::unnest: an empty or
    /// null list drops the row unless `keep_empty`, and a List<Struct>
    /// flattens into one column per field. schema() names the fields when the
    /// column's type is known at build time; otherwise it is empty until
    /// collect(). A NotFound error (kept as @ref status()) for an absent
    /// column and an InvalidArgument one for a non-List one, when the schema
    /// can tell.
    LazyFrame unnest(dstd::String column, bool keep_empty = false) const;
    /// Reshape wide -> long: keep `id_vars`, stack `value_vars` into a
    /// variable/value column pair.
    LazyFrame unpivot(dstd::Vector<dstd::String> id_vars,
                      dstd::Vector<dstd::String> value_vars) const;
    /// The `k` rows with the largest (or smallest) `name` values.
    LazyFrame topk(dstd::String name, std::int64_t k,
                   bool largest = true) const;
    /// Group by `key` and compute each aggregate (streaming: one partial state,
    /// bounded by the group count).
    LazyFrame group_by(dstd::String key, dstd::Vector<GroupAgg> aggs) const;
    /// Group by N key columns (a composite key: hashed and compared
    /// column-by-column, each keeping its own type). Streaming, as the
    /// single-key overload. `dyn` (optional) enables the name-keyed dyn
    /// side-table: each morsel's columns whose name starts with `dyn_prefix`,
    /// declared or undeclared by the source, are
    /// folded through agg_accumulate's dyn feed, with `dyn_prefix` stripped
    /// from each name. The dyn output columns are appended after the fixed
    /// aggregates.
    LazyFrame group_by(dstd::Vector<dstd::String> keys,
                       dstd::Vector<GroupAgg> aggs,
                       dstd::Vector<AggDynSpec> dyn = {},
                       dstd::String dyn_prefix = {}) const;
    /// The specs that broadcast `agg` over this plan's schema as it stands
    /// (batch_ops reduce_specs over the typed schema; a column of unknown
    /// type is included and refused at collect if it turns out non-numeric).
    /// An InvalidArgument error for an `agg` that does not broadcast, and a
    /// NotFound one for a key the schema lacks.
    dstd::Result<dstd::Vector<GroupAgg>> reduce_specs(
        Agg agg, const dstd::Vector<dstd::String>& keys = {}) const;
    /// DataFrame::reduce as a plan step: the streaming one-group group_by
    /// with `agg` broadcast over the eligible columns. The grouped form is
    /// group_by(keys).reduce(agg).
    LazyFrame reduce(Agg agg) const;
    /// The two-step form: the keys now, the aggregates on the returned
    /// @ref LazyGroupBy (`lf.group_by({"k"}).sum()`, `.agg(specs)`), each a
    /// streaming group_by step. Empty keys make the whole plan one group.
    LazyGroupBy group_by(dstd::Vector<dstd::String> keys) const;
    /// Group by an expression key and compute each expression aggregate.
    /// Desugars to with_column + the string group_by: a bare column-ref key or
    /// aggregate value is used directly, a computed one is materialized into a
    /// hidden temp column first. Fully streaming, same as the string overload.
    LazyFrame group_by(Expr key, dstd::Vector<AggExprSpec> aggs) const;
    /// Group by N expression keys; each desugars like the single-key overload.
    LazyFrame group_by(dstd::Vector<Expr> keys,
                       dstd::Vector<AggExprSpec> aggs) const;
    /// Sort by `name`. External merge sort: spills sorted runs past the memory
    /// budget and k-way merges them, so peak memory stays bounded.
    LazyFrame sort_by(dstd::String name, bool descending = false) const;
    /// Stable lexicographic sort by several key columns (nulls last), as
    /// DataFrame::sort_by_multi. Not streaming: needs every row to compare
    /// across the whole frame.
    LazyFrame sort_by_multi(dstd::Vector<dstd::String> by,
                            bool descending = false) const;
    /// Per-column direction form: `descending[i]` applies to `by[i]`.
    LazyFrame sort_by_multi(dstd::Vector<dstd::String> by,
                            dstd::Vector<bool> descending) const;
    /// Distinct rows, first occurrence, in original order, keyed on every
    /// column or on `subset` (empty = all). Streams input and output; holds
    /// only the distinct key set. A NotFound error (kept as @ref status()) for
    /// a subset name the schema lacks, when the schema is known.
    LazyFrame unique(dstd::Vector<dstd::String> subset = {}) const;
    /// Alias of unique().
    LazyFrame drop_duplicates(dstd::Vector<dstd::String> subset = {}) const;
    /// A deterministic n-row sample (mix64 min-hash). Streaming: bounded to n
    /// rows regardless of input size.
    LazyFrame sample(std::int64_t n, std::uint64_t seed = 0) const;
    /// For each distinct `keys` tuple, the first `n` rows with that key, in
    /// input order. A null key value is its own key, and equal numbers are one
    /// key whatever their numeric type. Empty `keys` is head(n); n <= 0 keeps
    /// no rows. Streams input and output; holds one counter per distinct key.
    /// A NotFound error (kept as @ref status()) for a key the schema lacks,
    /// when the schema is known.
    LazyFrame head_by(dstd::Vector<dstd::String> keys, std::int64_t n) const;
    /// One Bool column: true where the whole row is duplicated. Two-pass
    /// (count, then per-row mask in input order); state is the count map.
    LazyFrame is_duplicated() const;
    /// One Bool column: true where the whole row is unique. Two-pass, as
    /// is_duplicated.
    LazyFrame is_unique() const;
    /// Tumbling/sliding time-window aggregation over an ascending Int64 time
    /// column, as DataFrame::group_by_dynamic. Streaming: a window leaves once
    /// a time past its end is seen, so it holds only the open windows. The
    /// collect returns an InvalidArgument error when the time goes back.
    LazyFrame group_by_dynamic(dstd::String time_col, std::int64_t every,
                               std::int64_t period, dstd::Vector<GroupAgg> aggs,
                               std::int64_t origin = 0,
                               bool origin_min = false) const;
    /// unpivot alias.
    LazyFrame melt(dstd::Vector<dstd::String> id_vars,
                   dstd::Vector<dstd::String> value_vars) const;
    /// Hash join with `other` on `left_on[i]` = `right_on[i]`, as
    /// DataFrame::join. `other` is collected in full when this plan runs (the
    /// build side, bounded by the right row count); this plan streams through
    /// it morsel by morsel, so Inner / Left / Semi / Anti / Cross / Lookup /
    /// Nest hold no left state and Right / Outer add one match bit per right
    /// row. An optimizer barrier: no filter or projection moves across it.
    /// An absent key, a key type mismatch or a Lookup conflict is reported at
    /// collect(). When the build side outgrows memory_budget() (any kind but
    /// Cross), both sides are partitioned on the key to disk and joined one
    /// partition at a time: the same rows and columns in no particular row
    /// order. With `nulls_equal` a null key cell matches another null key cell
    /// (never a value); it is refused for Cross, Lookup and Nest, and such a
    /// join is not offered to a source's apply_join.
    LazyFrame join(LazyFrame other, dstd::Vector<dstd::String> left_on,
                   dstd::Vector<dstd::String> right_on,
                   JoinHow how = JoinHow::Inner, dstd::String suffix = "_right",
                   bool nulls_equal = false) const;
    /// Join on the same-named key columns `on`.
    LazyFrame join(LazyFrame other, dstd::Vector<dstd::String> on,
                   JoinHow how = JoinHow::Inner, dstd::String suffix = "_right",
                   bool nulls_equal = false) const;
    /// Vertical concatenation: every row of this plan, then every row of
    /// `other`. Both plans must have the same column names in the same order
    /// with the same types where known; a mismatch is an InvalidArgument
    /// error, kept as @ref status(). Streams both sides in turn with no state.
    /// An optimizer barrier: no filter or projection moves across it.
    LazyFrame concat(LazyFrame other) const;
    /// DataFrame::compare_agg over the two collected plans: keys, `l_`/`r_`
    /// per metric, then `delta_`/`pct_` per numeric metric. Output names are
    /// data-dependent, so schema() is empty until collect(). Runs through
    /// frame_op("dftu.frame.compare_agg").
    LazyFrame compare_agg(LazyFrame variant, std::int64_t n_key) const;
    /// DataFrame::asof over the two plans: each row of this plan takes the one
    /// row of `other` nearest in the numeric time column `on` within the `by`
    /// partition (Backward, Forward or Nearest; a null or NaN time or one past
    /// `tolerance`, in the units of `on`, matches nothing). The output keeps
    /// every column of this plan, then those of `other` but `on` and `by`
    /// (a clash gets "_right"), rows ordered by (by, time) with ties in input
    /// order. Both sides are sorted by (by, time) with the spilling sort and
    /// merged under the plan's budget (the automatic one when none is set),
    /// holding only the right rows of the current by-group window; over a plan
    /// whose columns are only known once it runs, both are collected. An
    /// InvalidArgument error (kept as @ref status()) for a negative or NaN
    /// `tolerance`, unequal or non-numeric key types or a second name clash,
    /// and a NotFound one for a missing column, when the schemas are known
    /// here.
    LazyFrame asof(LazyFrame other, dstd::String on,
                   dstd::Vector<dstd::String> by, AsofDirection direction,
                   dstd::Optional<double> tolerance = dstd::nullopt) const;
    /// DataFrame::interval over the two plans: each point of `point` pairs with
    /// every row of `other` whose closed range [`lo`, `hi`] holds it within
    /// the `by` partition, ordered by (by, point) then (lo, hi, input order);
    /// `outer` keeps an unmatched row once with null right values. The output
    /// keeps every column of this plan, then those of `other` but `lo`, `hi`
    /// and `by`. Runs as asof does: sorted and merged under the plan's budget,
    /// holding the ranges that are open at the current point (the output of
    /// one point is cut into morsels within the budget).
    LazyFrame interval(LazyFrame other, dstd::String point, dstd::String lo,
                       dstd::String hi, dstd::Vector<dstd::String> by,
                       bool outer = false) const;
    /// Any registry table -> table op (a `dftu.frame.*` row, or one another
    /// library registered, such as `dftu.frame.window`) as a plan step: this
    /// plan is collected and passed as the op's first FRAME operand, each
    /// further FRAME operand takes the next plan of `others` (collected when
    /// the op runs), every other operand is deep-copied from `args` now so
    /// the plan owns it. A pipeline breaker and an optimizer barrier. The
    /// output names are `out_names` when the caller can state them from the
    /// op's contract (a window keeps its input and appends its specs), else
    /// data-dependent (schema() empty until collect()). An InvalidArgument
    /// error, kept as @ref status(), if no such op is registered, it is not a
    /// table -> table op, an operand has no owned form (EXPR / DUQL / LAZY)
    /// or `others` does not match the op's frame operands.
    LazyFrame frame_op(dstd::String name, OpArgs args,
                       dstd::Vector<LazyFrame> others = {},
                       dstd::Vector<dstd::String> out_names = {}) const;
    /// Reshape long -> wide. Two-pass (distinct index+on values, then cell
    /// aggregation via the agg IR); state is bounded by the output (index rows
    /// x on-values). Output columns are data-dependent (one per distinct `on`
    /// value), so schema() is empty until collect().
    LazyFrame pivot(dstd::String index, dstd::String on, dstd::String values,
                    dstd::String agg = "first") const;
    /// One-hot encode `column`. Two-pass (distinct values, then stream-encode);
    /// state is bounded by the value cardinality. Output columns are
    /// data-dependent, so schema() is empty until collect().
    LazyFrame to_dummies(dstd::String column) const;
    /// Per-column summary statistics (count/null_count/mean/std/min/max).
    /// Streaming, constant state per column; output columns are data-dependent
    /// (one per numeric input column), so schema() is empty until collect().
    LazyFrame describe() const;

    /// Out-of-core budget for the pipeline breakers (sort/unique/group_by):
    /// when a sink's in-memory state grows past this many bytes it spills to a
    /// sorted temp run, k-way merged at the end, so peak memory stays bounded.
    /// 0 (the default) means "auto": ~1/3 of available memory. Pass
    /// memory::NO_SPILL_BUDGET to disable spilling.
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
    /// An InvalidArgument error, kept as @ref status(), if no node is
    /// registered under `name`. The node is an optimization barrier: no filter
    /// or projection is ever pushed through it, and the engine never reorders
    /// around it. `args` is copied by value; a pointer-bearing operand it
    /// carries must outlive every future execution of the returned
    /// @ref LazyFrame, not merely this call.
    LazyFrame op(dstd::String name, OpArgs args) const;

    /// The first error a builder hit (an unknown column, a bad join kind, a
    /// concat whose columns differ, ...), or an empty result. The builders
    /// keep it and every later builder keeps passing it on, so a chain reads
    /// as before; every terminal (collect(), stream(), open_cursor(), ...)
    /// returns it without running anything. A plan that holds an error still
    /// reports the schema as it stood before the failing step.
    dstd::Result<void> status() const;

    /// Output column names without running the query. Empty for a plan ending
    /// in a data-dependent op (pivot/to_dummies/describe).
    dstd::Vector<dstd::String> schema() const;

    /// The typed output schema without running the query: walks the plan from
    /// the source's Schema, transforming it per op the same way schema()
    /// transforms names. Empty for a data-dependent op (pivot/to_dummies/
    /// describe), matching schema(). A field whose type an op cannot
    /// determine statically is TypeId::Unknown, never a guess.
    Schema output_schema() const;

    /// The optimized plan as text (source then one op per line), for
    /// introspection and tests.
    dstd::String explain() const;

    /// Pull-based chunk generator: the streaming terminal. Each yielded
    /// @ref DataFrame is standalone, carrying its own schema. One-shot: re-run
    /// from the @ref LazyFrame to restart. `batch_size` is the scan chunk
    /// size;
    /// <= 0 means auto. collect() drains this. A chunk that cannot be made is
    /// an error result, after which the generator ends; so is a plan whose
    /// cursor chain cannot be built or that holds a @ref status() error.
    coro::AsyncGenerator<dstd::Result<DataFrame>> stream(
        std::int64_t batch_size = 0) const;

    /// The plan's cursor chain, undrained, so a caller can read each morsel's
    /// ordering before stream() materializes it away. One-shot, as stream().
    /// The error is the one stream() would end with first.
    dstd::Result<dstd::UniquePtr<Cursor>> open_cursor() const;

    /// Run the pipeline and materialize the surviving rows. `batch_size` is
    /// the scan chunk size; <= 0 (the default) means auto; one pass over a
    /// resident source, the bounded streaming default otherwise. The error is
    /// the first one a builder kept (@ref status()) or the one that ended the
    /// run: a column the data lacks, a type a kernel refuses, a spill file
    /// that cannot be written or read, a failed plugin node, or a cursor that
    /// threw (as an error of the exception's message).
    coro::CoroTask<dstd::Result<DataFrame>> collect(
        std::int64_t batch_size = 0) const;

    /// Group `keys` with `aggs` and return the mergeable partial instead of a
    /// finalized frame: streams this pipeline (no group op appended) and folds
    /// every morsel into one AggState. The caller finalizes (agg_finalize),
    /// coarsens (agg_regroup), or persists it. State is bounded by the distinct
    /// group count, not the input; used by the rollup materialize path. The
    /// errors are those of @ref collect.
    coro::CoroTask<dstd::Result<AggStatePtr>> collect_group_state(
        dstd::Vector<dstd::String> keys, dstd::Vector<GroupAgg> aggs,
        dstd::Vector<AggDynSpec> dyn = {}, dstd::String dyn_prefix = {},
        std::int64_t batch_size = 0) const;

   private:
    friend struct detail::PlanAccess;
    LazyFrame(dstd::SharedPtr<const Source> source,
              dstd::Vector<dstd::SharedPtr<const LazyOp>> ops,
              std::uint64_t memory_budget = 0,
              dstd::SharedPtr<const dstd::Error> error = nullptr)
        : source_(std::move(source)),
          ops_(std::move(ops)),
          memory_budget_(memory_budget),
          error_(std::move(error)) {}
    // Clone with a new op list, preserving the source, budget and error.
    LazyFrame with_ops(dstd::Vector<dstd::SharedPtr<const LazyOp>> ops) const {
        return LazyFrame(source_, std::move(ops), memory_budget_, error_);
    }
    // The ops over a resident frame, whole-column: an op with an eager form
    // runs it, any other runs its own cursor over the frame as one morsel.
    static coro::CoroTask<dstd::Result<DataFrame>> run_ops_in_memory(
        DataFrame df, dstd::Vector<dstd::SharedPtr<const LazyOp>> ops);
    // This plan with `error` kept as its first error, unless it holds one.
    LazyFrame failed(dstd::Error error) const;
    // This plan with the first error of `other` kept, unless it holds one.
    LazyFrame with_error_of(const LazyFrame& other) const;
    dstd::SharedPtr<const Source> source_;
    dstd::Vector<dstd::SharedPtr<const LazyOp>> ops_;
    std::uint64_t memory_budget_ = 0;
    dstd::SharedPtr<const dstd::Error> error_;
};

/// The two-step lazy group-by: a plan and its key columns, with the
/// aggregates chosen on it. Every method appends one streaming group_by step
/// (the same GroupByOp LazyFrame::group_by(keys, specs) records); the family
/// below broadcasts one aggregate over the eligible non-key columns.
class LazyGroupBy {
   public:
    LazyGroupBy(LazyFrame plan, dstd::Vector<dstd::String> keys);

    const dstd::Vector<dstd::String>& keys() const noexcept { return keys_; }

    LazyFrame agg(dstd::Vector<GroupAgg> aggs) const;
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
    /// pandas' `first`). An InvalidArgument error when
    /// no column is eligible, NGroup has no keys, the window is under 1 or the
    /// window op is not registered, and a NotFound error for an absent key.
    dstd::Result<LazyFrame> transform(GroupwiseOp kind, std::int64_t n = 0,
                                      RankMethod method = RankMethod::Average,
                                      bool ascending = true) const;
    dstd::Result<LazyFrame> cumsum() const {
        return transform(GroupwiseOp::CumSum);
    }
    dstd::Result<LazyFrame> cummax() const {
        return transform(GroupwiseOp::CumMax);
    }
    dstd::Result<LazyFrame> cummin() const {
        return transform(GroupwiseOp::CumMin);
    }
    dstd::Result<LazyFrame> cumcount() const {
        return transform(GroupwiseOp::CumCount);
    }
    dstd::Result<LazyFrame> shift(std::int64_t periods = 1) const {
        return transform(GroupwiseOp::Shift, periods);
    }
    dstd::Result<LazyFrame> diff() const {
        return transform(GroupwiseOp::Diff);
    }
    dstd::Result<LazyFrame> pct_change() const {
        return transform(GroupwiseOp::PctChange);
    }
    dstd::Result<LazyFrame> rank(RankMethod method = RankMethod::Average,
                                 bool ascending = true) const {
        return transform(GroupwiseOp::Rank, 0, method, ascending);
    }
    dstd::Result<LazyFrame> ngroup() const {
        return transform(GroupwiseOp::NGroup);
    }
    dstd::Result<LazyFrame> head(std::int64_t n = 5) const {
        return transform(GroupwiseOp::Head, n);
    }
    dstd::Result<LazyFrame> tail(std::int64_t n = 5) const {
        return transform(GroupwiseOp::Tail, n);
    }
    dstd::Result<LazyFrame> nth(std::int64_t n) const {
        return transform(GroupwiseOp::Nth, n);
    }
    dstd::Result<LazyFrame> cumprod() const {
        return transform(GroupwiseOp::CumProd);
    }
    dstd::Result<LazyFrame> ffill() const {
        return transform(GroupwiseOp::FFill);
    }
    dstd::Result<LazyFrame> bfill() const {
        return transform(GroupwiseOp::BFill);
    }
    dstd::Result<LazyFrame> rolling_sum(std::int64_t window) const {
        return transform(GroupwiseOp::RollingSum, window);
    }
    dstd::Result<LazyFrame> rolling_mean(std::int64_t window) const {
        return transform(GroupwiseOp::RollingMean, window);
    }
    dstd::Result<LazyFrame> rolling_min(std::int64_t window) const {
        return transform(GroupwiseOp::RollingMin, window);
    }
    dstd::Result<LazyFrame> rolling_max(std::int64_t window) const {
        return transform(GroupwiseOp::RollingMax, window);
    }

   private:
    LazyFrame plan_;
    dstd::Vector<dstd::String> keys_;
};

/// Free-function form of DataFrame::lazy(), for `lazy(df)` call sites.
LazyFrame lazy(DataFrame frame);

/// Collect every plan, one @ref DataFrame per plan in input order. After each
/// plan is optimized, plans whose sources share a Source::batch_key() read
/// their data in one Source::collect_batch() call; the rest collect as
/// collect() would. The first error of any plan is the result.
coro::CoroTask<dstd::Result<dstd::Vector<DataFrame>>> collect_all(
    dstd::Vector<LazyFrame> plans);

/// A lazy result that is not one frame: the plans it reads, collected
/// together, and the step that turns their frames into a `T`. A source-specific
/// terminal (a trace flamegraph partial, a sink's stats) is one, and
/// collect_all() batches its plans with every other root's. The finish step is
/// the caller's code and may throw; collect() runs in the caller's translation
/// unit.
template <class T>
class LazyResult {
   public:
    using value_type = T;
    /// A shared, copyable handle on the step that turns frames into a `T`.
    class Finish {
       public:
        using Fn = dstd::Function<coro::CoroTask<T>(dstd::Vector<DataFrame>)>;

        template <class F>
            requires(!std::is_same_v<std::decay_t<F>, Finish> &&
                     std::is_invocable_r_v<coro::CoroTask<T>, std::decay_t<F>&,
                                           dstd::Vector<DataFrame>>)
        Finish(F&& f) : fn_(dstd::make_shared<Fn>(std::forward<F>(f))) {}

        coro::CoroTask<T> operator()(dstd::Vector<DataFrame> frames) const {
            return (*fn_)(std::move(frames));
        }

       private:
        dstd::SharedPtr<Fn> fn_;
    };

    LazyResult(dstd::Vector<LazyFrame> plans, Finish finish)
        : plans_(std::move(plans)), finish_(std::move(finish)) {}

    const dstd::Vector<LazyFrame>& plans() const { return plans_; }
    const Finish& finish() const { return finish_; }

    /// The value of the finish step, or the first error of a plan.
    coro::CoroTask<dstd::Result<T>> collect() const {
        return run(plans_, finish_);
    }

   private:
    static coro::CoroTask<dstd::Result<T>> run(dstd::Vector<LazyFrame> plans,
                                               Finish finish) {
        dstd::Vector<DataFrame> frames;
        if (!plans.empty()) {
            auto collected = co_await collect_all(std::move(plans));
            if (!collected) co_return collected.error();
            frames = std::move(*collected);
        }
        T value = co_await finish(std::move(frames));
        co_return std::move(value);
    }

    dstd::Vector<LazyFrame> plans_;
    Finish finish_;
};

namespace detail {

template <class T>
coro::CoroTask<T> ready(T value) {
    co_return value;
}

inline LazyResult<DataFrame> as_result(LazyFrame plan) {
    return {{std::move(plan)}, [](dstd::Vector<DataFrame> frames) {
                return ready(std::move(frames.front()));
            }};
}

template <class T>
LazyResult<T> as_result(LazyResult<T> result) {
    return result;
}

template <class R>
using ResultOfRoot =
    typename decltype(as_result(std::declval<R>()))::value_type;

template <std::size_t I, class Results, class Out>
coro::CoroTask<void> finish_roots(Results& results,
                                  dstd::Vector<DataFrame>& frames,
                                  std::size_t at, Out& out) {
    if constexpr (I < std::tuple_size_v<Results>) {
        auto& r = std::get<I>(results);
        const std::size_t n = r.plans().size();
        dstd::Vector<DataFrame> mine(
            std::make_move_iterator(frames.begin() + at),
            std::make_move_iterator(frames.begin() + at + n));
        std::get<I>(out) = co_await r.finish()(std::move(mine));
        co_await finish_roots<I + 1>(results, frames, at + n, out);
    }
}

}  // namespace detail

/// Collect @ref LazyFrame and @ref LazyResult roots together, one value per
/// root in argument order: a @ref DataFrame for a @ref LazyFrame, a `T` for a
/// LazyResult<T>. Every root's plans batch as collect_all(plans) batches them.
/// The first error of any plan is the result.
template <class... R>
coro::CoroTask<dstd::Result<dstd::Tuple<detail::ResultOfRoot<R>...>>>
collect_all(R... roots) {
    auto results = std::make_tuple(detail::as_result(std::move(roots))...);
    dstd::Vector<LazyFrame> plans;
    std::apply(
        [&](const auto&... r) {
            (plans.insert(plans.end(), r.plans().begin(), r.plans().end()),
             ...);
        },
        results);
    dstd::Vector<DataFrame> frames;
    if (!plans.empty()) {
        auto collected = co_await collect_all(std::move(plans));
        if (!collected) co_return collected.error();
        frames = std::move(*collected);
    }
    dstd::Tuple<detail::ResultOfRoot<R>...> out;
    co_await detail::finish_roots<0>(results, frames, 0, out);
    co_return std::move(out);
}

/// Lower name-based GroupAggs to index-based AggSpecs plus the deduped list of
/// value column names they reference (Count references none); the i-th spec's
/// value_col/by_col index into `value_names`. The gagg lowering that
/// collect_group_state and any external chunk driver share, so a Fold feeding
/// its own chunks accumulates into a byte-identical AggState.
struct LoweredGroupAggs {
    dstd::Vector<AggSpec> specs;
    dstd::Vector<dstd::String> value_names;
};
LoweredGroupAggs lower_group_aggs(const dstd::Vector<GroupAgg>& aggs);

/// Accumulate one already-built chunk `frame` into `state`: resolve `keys` and
/// `value_names` (from lower_group_aggs) by name against `frame`, feeding every
/// column whose name starts with `dyn_prefix` as a dyn input (prefix stripped).
/// The per-morsel step collect_group_state runs, exposed for a non-@ref
/// LazyFrame chunk driver (a trace Fold accumulating over a shared scan).
dstd::Result<void> agg_accumulate_chunk(
    AggState& state, const DataFrame& frame,
    const dstd::Vector<dstd::String>& keys,
    const dstd::Vector<dstd::String>& value_names,
    const dstd::String& dyn_prefix);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_LAZYFRAME_H
