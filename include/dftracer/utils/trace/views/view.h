#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_H

#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/field.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/trace_config.h>
#include <dftracer/utils/trace/views/result_join.h>
#include <dftracer/utils/utilities/common/statistics/ddsketch.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dftracer::utils {
class StringIntern;
}

namespace dftracer::utils::trace::indexing {
class BloomFilterCache;
}

namespace dftracer::utils::json {
class JsonValue;
}

namespace dftracer::utils::trace::views {

using dftracer::utils::dataframe::field::FieldAggExpr;
using dftracer::utils::dataframe::field::FieldExpr;
using query::Query;

/// `ph="X"` events, `ph="C"` counters, `ph="A"` aggregated (folded events),
/// `ph="M"` metadata, or all (`Any`).
enum class Phase { Events, Counters, Aggregated, Metadata, Any };

/// One column of a (possibly composite) group-by key. `Arg` groups on an
/// args-map entry named by `arg`; the rest group on the like-named field.
struct GroupKey {
    /// Fhash/Hhash group on the args file/host hash; IoCat is the dfanalyzer
    /// I/O category derived from the function name; AccPat is the access
    /// pattern (0 today). FilePath/HostName group on the same hash as
    /// Fhash/Hhash but the group key is relabeled to the resolved name after
    /// aggregation (a bijection, so grain is identical), keeping the fold
    /// hash-only.
    enum class Kind {
        Name,
        Cat,
        Pid,
        Tid,
        Fhash,
        Hhash,
        IoCat,
        AccPat,
        FilePath,
        FileName,
        HostName,
        /// Group by pid, relabeled to the rank from the trace's PR metadata.
        /// rank is stable across runs where pid is not.
        Rank,
        Arg,
        /// Any field by name, resolved top-level then args; `arg` holds the
        /// name. Unlike Arg (args-only) this also sees top-level fields.
        Field
    };
    /// Value transform applied to the resolved group value, before the
    /// merge key is built. Coarsens the grain (many values fold to one), so
    /// it must be part of any cache identity derived from the plan.
    enum class Transform { None, Dirname, Basename, Lower, Bucket };

    Kind kind = Kind::Name;
    std::string arg;
    Transform transform = Transform::None;
    /// Bucket substrings, in priority order; the first one contained in the
    /// value wins and values matching none fold to empty. Bucket only.
    std::vector<std::string> transform_args{};

    static GroupKey name() { return {Kind::Name, {}}; }
    static GroupKey cat() { return {Kind::Cat, {}}; }
    static GroupKey pid() { return {Kind::Pid, {}}; }
    static GroupKey tid() { return {Kind::Tid, {}}; }
    static GroupKey fhash() { return {Kind::Fhash, {}}; }
    static GroupKey hhash() { return {Kind::Hhash, {}}; }
    static GroupKey io_cat() { return {Kind::IoCat, {}}; }
    static GroupKey acc_pat() { return {Kind::AccPat, {}}; }
    static GroupKey file_path() { return {Kind::FilePath, {}}; }
    static GroupKey file_name() { return {Kind::FileName, {}}; }
    static GroupKey host_name() { return {Kind::HostName, {}}; }
    static GroupKey rank() { return {Kind::Rank, {}}; }
    static GroupKey of_arg(std::string key) {
        return {Kind::Arg, std::move(key)};
    }
    static GroupKey field(std::string name) {
        return {Kind::Field, std::move(name)};
    }
};

/// Var/Std are population variance / standard deviation of `field`, over the
/// same N as Mean (the group's event count). Pct is the `q`-quantile of `field`
/// from a DDSketch (q in (0,1)).
enum class AggOp {
    Count,
    Sum,
    Min,
    Max,
    Mean,
    Var,
    Std,
    ArgMax,
    SumSq,
    Pct,
    Skew,      ///< population skewness of `field`
    Kurt,      ///< population excess kurtosis of `field`
    Hist,      ///< raw histogram of `field` from a DDSketch
               ///< (list<struct<lo,hi,count>>)
    SetUnion,  ///< distinct string values of `field`, emitted as a delimiter-
               ///< joined text column (sorted)
    Busy,      ///< occupancy: wall-clock us at least one event was active
               ///< (exact interval union)
    Concurrency,  ///< average parallelism: sum(dur) / busy
    Utilization,  ///< busy / makespan (max_end - min_ts)
    Active        ///< peak concurrent headcount (exact max overlap depth)
};

/// True for the occupancy ops (busy/concurrency/utilization/active): time-
/// interval reductions over [ts, ts+dur), computed in the scan, with no value
/// field.
inline bool is_occupancy_op(AggOp op) {
    return op == AggOp::Busy || op == AggOp::Concurrency ||
           op == AggOp::Utilization || op == AggOp::Active;
}

/// True for the reductions runnable over every discovered numeric arg
/// (agg_numeric_args). Count/Sum/Min/Max/SumSq/Mean/Var/Std/Skew/Kurt come from
/// the per-arg FieldStat; Pct additionally collects a per-arg DDSketch. Hist,
/// ArgMax, SetUnion and occupancy need per-event or per-field state, so those
/// stay on a named field.
inline bool is_dyn_reduction(AggOp op) {
    return op == AggOp::Count || op == AggOp::Sum || op == AggOp::Min ||
           op == AggOp::Max || op == AggOp::SumSq || op == AggOp::Mean ||
           op == AggOp::Var || op == AggOp::Std || op == AggOp::Skew ||
           op == AggOp::Kurt || op == AggOp::Pct;
}

/// `field` is the field to reduce (ignored for `Count()`; for `ArgMax` it is
/// the value returned, e.g. "name"). `by` is the numeric field maximized over
/// for `ArgMax` (e.g. "dur"). `q` is the quantile for `Pct` (e.g. 0.99).
/// `out_name` names the output column.
struct AggSpec {
    AggOp op = AggOp::Count;
    std::string field;
    std::string out_name;
    std::string by;  ///< ArgMax only: the field whose max selects `field`
    double q = 0.0;  ///< Pct only: the quantile in (0, 1)

    AggSpec() = default;
    AggSpec(AggOp o, std::string f = "", std::string out = "",
            std::string b = "", double quant = 0.0)
        : op(o),
          field(std::move(f)),
          out_name(std::move(out)),
          by(std::move(b)),
          q(quant) {}
};

/// A file and its index. Sizes are optional; zero means read them from the
/// index.
struct ViewFile {
    std::string file_path;
    std::string index_path;
    std::uint64_t uncompressed_size = 0;
    std::size_t num_checkpoints = 0;
    std::size_t checkpoint_size = 0;  ///< 0 = indexer default
};

struct ExportStats {
    std::uint64_t events_matched = 0;
    std::uint64_t events_scanned = 0;
    std::uint64_t chunks_scanned = 0;
    std::uint64_t chunks_skipped = 0;
    /// Chunks read end to end. Below chunks_scanned when the scan stopped
    /// early, so a chunk was touched but not wholly seen.
    std::uint64_t chunks_covered = 0;
    /// Every index artifact riding along on this scan persisted. False when
    /// none was attached, or when one declined (nothing to record, over
    /// budget, a prerequisite artifact missing).
    bool artifacts_committed = false;
    bool truncated = false;  ///< scan stopped early on a limit
    /// This read was served from a materialized view instead of the base
    /// traces (the scan ran over a subsuming filtered-trace MV).
    bool served_from_mv = false;
};

/// Options for export_trace(): write the View's events as a new re-indexable
/// multi-member trace through the parallel (PFS-aware) writer. `member_size` is
/// the target uncompressed bytes per gzip member (0 = default); `num_workers`
/// caps the writer fan-out. The writer layout (striped/sharded/padded) and the
/// exact worker count are chosen from the target filesystem.
struct TraceWriteOptions {
    std::string output_path;
    std::size_t member_size = 0;
    std::size_t num_workers = 1;
    bool compress = true;
    int level = 6;
    /// Build the member+bloom+stats index during the write (no re-inflate of
    /// the output). `index_path` is the index DB directory; empty falls back to
    /// the indexer default location for `output_path`. When set, the write uses
    /// a single in-order member stream (scan/decompress stays parallel) so the
    /// index needs no post-hoc offset remap.
    bool build_index = false;
    std::string index_path;
    /// Roll to a new `part-<k>` file once a part reaches this many uncompressed
    /// bytes, so one large result splits into several indexed part files
    /// sharing `index_path`. 0 = one file. Only honored on the indexed write
    /// path.
    std::size_t part_size = 0;
};

/// Streaming output target. The executor serializes each `write` call, so an
/// implementation only needs to handle one call at a time.
class ExportSink {
   public:
    virtual ~ExportSink() = default;
    virtual void write(std::string_view data) = 0;
};

/// The three record families of one aggregation index, read in a single pass:
/// `regular` (ph="X" events), `aggregated` (aggregated records carrying
/// arbitrary extra-key dims), and `counters` (ph="C" counters incl. system).
/// Returned by collect_typed().
struct TypedResult {
    dftracer::utils::dataframe::DataFrame regular;
    dftracer::utils::dataframe::DataFrame aggregated;
    dftracer::utils::dataframe::DataFrame counters;
};

/// Progress callback for a shard-range read: called with (done, total) shard
/// units as the scan advances through the range.
using ProgressFn = std::function<void(std::size_t, std::size_t)>;

/// Result of map_batches(): the reduced fold `value` plus the `stats` of the
/// scan that produced it.
template <class P>
struct BatchResult {
    P value;
    ExportStats stats;
};

namespace detail {
/// Opaque internal types (view_plan.h / view_aggregate.h / fold.h /
/// view_executor.h); they appear below only as pimpl-style handles or
/// advanced-method parameters. The scan/session engine seams the inline
/// templates below build on are private members of View/ViewSession, defined
/// in the .cpp - no engine surface is exposed in this public header.
struct ViewPlan;
class Fold;
class DynamicPrune;
struct ViewSessionState;

/// Phase 1 of the View -> dataframe engine aggregation convergence
/// (view_agg_engine.h/.cpp); needs View's private constructor to build the
/// raw row-query LazyFrame the streaming group_by runs over.
coro::CoroTask<dftracer::utils::dataframe::DataFrame> run_collect_via_engine(
    const ViewPlan& plan);

// Builds the engine group-by inputs (and the raw scan View) for a plan; needs
// View's private constructor.
struct EnginePrep;
coro::CoroTask<EnginePrep> prepare_engine_group(const ViewPlan& plan);
}  // namespace detail

/// A result handle from a ViewSession op, resolved when execute() completes.
/// Reading it (get / * / ->) before execute() throws, since the value does not
/// exist yet. operator bool reports whether an op was registered; a
/// default-constructed handle is empty.
template <class T>
class Deferred {
   public:
    Deferred() = default;
    Deferred(std::shared_ptr<T> value, std::shared_ptr<const bool> executed)
        : value_(std::move(value)), executed_(std::move(executed)) {}

    explicit operator bool() const { return static_cast<bool>(value_); }

    T& get() const {
        if (!value_ || !executed_ || !*executed_)
            throw std::logic_error(
                "ViewSession result read before execute() resolved it");
        return *value_;
    }
    T& operator*() const { return get(); }
    T* operator->() const { return &get(); }

   private:
    friend class ViewSession;
    std::shared_ptr<T> value_;
    std::shared_ptr<const bool> executed_;
};

class View;
class ViewSource;

/// The two handles a single containment branch yields from one buffered fold.
struct ContainmentHandles {
    Deferred<dftracer::utils::dataframe::DataFrame> call_tree;
    Deferred<dftracer::utils::dataframe::DataFrame> flamegraph;
};

/// A batch of read ops over one shared scan of a base View. Register ops
/// (collect/materialize/fold/export_json) - each returns a Deferred handle -
/// then execute() runs them together, serving a materialized aggregate from its
/// rollup and scanning only the rest. Unlike View::collect(), which runs at
/// once, the ops here resolve only on execute().
class ViewSession {
   public:
    /// Aggregate the branch's matching events (group_by + agg) into a
    /// DataFrame.
    Deferred<dftracer::utils::dataframe::DataFrame> collect(
        Query predicate, std::vector<GroupKey> group_by,
        std::vector<AggSpec> agg);

    /// Aggregate ALL of the base view's events (no per-branch predicate) into a
    /// DataFrame. For a branch that wants every scanned event, so several
    /// distinct group_by/agg aggregations share the one scan.
    Deferred<dftracer::utils::dataframe::DataFrame> collect(
        std::vector<GroupKey> group_by, std::vector<AggSpec> agg);

    /// Register an aggregation view as a branch, carrying its full plan.
    /// `branch` must be built off this session's base view.
    Deferred<dftracer::utils::dataframe::DataFrame> collect(const View& branch);

    /// Aggregate `branch` into a serialized partial (opaque bytes) for a
    /// distributed merge, over the shared scan. Merge and resolve later.
    Deferred<std::string> aggregate_partial(const View& branch);

    /// Persist the (group_by + agg) aggregation over ALL scanned events as a
    /// rollup, sharing the one scan. Build-only (no handle); a later matching
    /// collect() reads it back.
    void materialize(std::vector<GroupKey> group_by, std::vector<AggSpec> agg);

    /// Stream the branch's matching events verbatim to `sink`.
    Deferred<ExportStats> export_json(Query predicate, ExportSink& sink);

    /// Stream every scanned event to `sink`, sharing the session's scan.
    Deferred<ExportStats> export_json(ExportSink& sink);

    /// Materialize the branch's matching events into a DataFrame over the
    /// shared scan. The whole matching set is held in memory, so filter to
    /// bound it.
    Deferred<dftracer::utils::dataframe::DataFrame> collect_events(
        const View& branch);

    /// Containment over the shared scan: the branch's events buffer into a lean
    /// per-event tuple, and the parallel per-lane nesting pass runs on execute.
    /// call_tree returns the events plus level/parent_id; flamegraph returns
    /// the folded node frame. `partition`/`ts`/`dur`/`name` are named fields
    /// (POD scalar fast path, else captured arg/nested).
    Deferred<dftracer::utils::dataframe::DataFrame> call_tree(
        const View& branch, std::vector<std::string> partition = {"pid", "tid"},
        std::string ts = "ts", std::string dur = "dur",
        std::string name = "name");
    Deferred<dftracer::utils::dataframe::DataFrame> flamegraph(
        const View& branch, std::vector<std::string> partition = {"pid", "tid"},
        std::string ts = "ts", std::string dur = "dur",
        std::string name = "name");

    /// Both containment outputs from ONE buffered fold over the shared scan -
    /// the events buffer once and each lane sorts once, feeding both builders.
    ContainmentHandles containment(
        const View& branch, std::vector<std::string> partition = {"pid", "tid"},
        std::string ts = "ts", std::string dur = "dur",
        std::string name = "name");

    /// Equi-join two collect branches on their shared leading group keys, after
    /// the one scan. `left` and `right` must be collect() handles from this
    /// session that group the same way. `n_key` (the number of leading key
    /// columns) is inferred from `left`'s group_by when left as -1. Resolved on
    /// execute(), like every other handle here.
    Deferred<dftracer::utils::dataframe::DataFrame> join(
        Deferred<dftracer::utils::dataframe::DataFrame> left,
        Deferred<dftracer::utils::dataframe::DataFrame> right,
        JoinType how = JoinType::INNER, std::int64_t n_key = -1);

    /// FULL-join two collect branches on their shared leading group keys and
    /// append `delta_<m>`/`pct_<m>` per metric, after the one scan. `baseline`
    /// and `variant` must be collect() handles from this session that group and
    /// aggregate the same way. `n_key` is inferred from `baseline` when -1.
    Deferred<dftracer::utils::dataframe::DataFrame> compare(
        Deferred<dftracer::utils::dataframe::DataFrame> baseline,
        Deferred<dftracer::utils::dataframe::DataFrame> variant,
        std::int64_t n_key = -1);

    /// Fold the branch's matching events into a caller partial `P`, one per
    /// scan worker, reduced by `combine`. The fold gets the parsed event (no
    /// re-parse) and its raw JSON bytes (for verbatim passthrough). `P` must be
    /// default-constructible and a default `P` the identity of combine.
    template <class P>
    Deferred<P> fold(
        Query predicate,
        std::function<void(P&, const json::JsonValue&, std::string_view)> f,
        std::function<P(P&&, P&&)> combine) {
        auto out = std::make_shared<P>();
        auto partials = std::make_shared<std::vector<std::shared_ptr<P>>>();
        attach_fold(
            std::move(predicate),
            [f, partials]() {
                auto p = std::make_shared<P>();
                partials->push_back(p);
                return [f, p](const json::JsonValue& jv, std::string_view raw) {
                    f(*p, jv, raw);
                };
            },
            [partials, out, combine = std::move(combine)]() {
                P acc{};
                for (const auto& p : *partials)
                    acc = combine(std::move(acc), std::move(*p));
                *out = std::move(acc);
            });
        return {out, executed_};
    }

    /// fold() overload taking a unified field predicate (F("dur") > 1000).
    /// Throws if the predicate is not index-pushable.
    template <class P>
    Deferred<P> fold(
        const FieldExpr& predicate,
        std::function<void(P&, const json::JsonValue&, std::string_view)> f,
        std::function<P(P&&, P&&)> combine) {
        return fold<P>(predicate.to_query(), std::move(f), std::move(combine));
    }

    /// Attach an externally-built Fold to the shared scan. `make` constructs it
    /// with the scan's StringIntern so ids agree and per-worker slices merge;
    /// `finalize` runs after the scan while the fold is still alive. Forces a
    /// scan (a plugin cannot be served from a rollup).
    void attach_fold_factory(std::function<std::unique_ptr<detail::Fold>(
                                 dftracer::utils::StringIntern&)>
                                 make,
                             std::function<void()> finalize);

    /// Offer `q` as a narrowing of this session's shared scan, so the index can
    /// skip chunks the attached fold does not want. APPLIED ONLY when no other
    /// branch is registered: the scan feeds every branch, so narrowing it for
    /// one would starve the rest. execute() decides, because branches may be
    /// added after this call and only it knows the final set.
    void propose_base_prune(Query q);

    /// Scan the base once and run every attached branch, resolving every
    /// Deferred handle returned above.
    coro::CoroTask<ExportStats> execute();

   private:
    friend class View;
    explicit ViewSession(std::shared_ptr<const detail::ViewPlan> plan);
    /// Type-erased seam behind fold(): attach one branch to the run. The engine
    /// (BranchHooks etc.) stays internal; defined in the .cpp. `make_consumer`
    /// is called once per scan worker, serially, before the scan starts.
    void attach_fold(
        Query predicate,
        std::function<
            std::function<void(const json::JsonValue&, std::string_view)>()>
            make_consumer,
        std::function<void()> finalize);
    /// Look up the leading key-column count recorded for a collect branch's
    /// output, or -1 if that output was not a collect() of this session.
    std::int64_t key_count_of(const void* out) const;
    void add_containment_branch(
        const View& branch, const std::vector<std::string>& partition,
        const std::string& ts, const std::string& dur, const std::string& name,
        std::shared_ptr<dftracer::utils::dataframe::DataFrame> out_ct,
        std::shared_ptr<dftracer::utils::dataframe::DataFrame> out_fg);
    std::shared_ptr<detail::ViewSessionState> state_;
    std::shared_ptr<bool> executed_ = std::make_shared<bool>(false);
    /// Leading key-column count per collect branch, keyed by its output
    /// pointer, so join()/compare() can infer n_key without the caller passing
    /// it.
    std::vector<std::pair<const void*, std::int64_t>> key_counts_;
    /// Post-scan combines (join/compare), run after every branch finalize in
    /// execute() since they read two already-resolved branch outputs.
    std::vector<std::function<void()>> combines_;
};

class AggregatedView;
enum class JoinType;

/// A composable, lazy, relational view over trace events - the pandas/SQL-table
/// of a trace. Builder ops return a new `View` and do no I/O; terminals execute
/// the plan once, pushing filters into the index (bloom + chunk pruning) and
/// scanning candidates in parallel. The only public entry point; the
/// planner/scanner/aggregator engines it drives are internal.
class View {
   public:
    /// An empty view (no files); collects to an empty result. Also the
    /// placeholder a `coro::CoroTask<View>` needs before co_return.
    View();

    static View from_files(std::vector<ViewFile> files,
                           indexing::BloomFilterCache* bloom_cache = nullptr);
    static View from_file(std::string file_path, std::string index_path = "");

    /// Scan `dir` recursively for .pfw.gz files via the parallel directory
    /// scanner and build a View over them (sorted for determinism). Each file
    /// gets its own index, resolved under `index_path` (empty = the sidecar
    /// beside each trace, built on first touch). Self-contained; co_await it.
    static coro::CoroTask<View> from_directory(std::string dir,
                                               std::string index_path = "");

    /// Builder ops (lazy; each returns a new View).
    View filter(Query q) const;
    /// Filter by a unified field predicate (F("dur") > 1000). Throws if the
    /// expression is not index-pushable (a value op mixed into the predicate).
    View filter(const FieldExpr& pred) const { return filter(pred.to_query()); }
    View query(const std::string& dsl) const;
    View phase(Phase p) const;
    View time_range(double begin, double end) const;
    /// Bucket events into fixed `interval_us` windows aligned to 0 (absolute).
    View time_bucket(std::uint64_t interval_us) const;
    /// As above but align bucket boundaries to `origin_us` (in the post-scale
    /// unit): bucket i spans [origin + i*interval, origin + (i+1)*interval).
    View time_bucket(std::uint64_t interval_us, std::uint64_t origin_us) const;
    /// As above but align to the trace's minimum timestamp, resolved from the
    /// index zone maps (no scan) at execution. Best for a viewport whose trace
    /// starts at an arbitrary absolute time.
    View time_bucket_min(std::uint64_t interval_us) const;
    /// Target occupancy cell size (busy quantum) in us; 0 = engine default.
    /// Honored only with a time_range (see ViewPlan::occ_cell_us).
    View occ_cell(std::uint64_t cell_us) const;
    /// Normalize ts/dur/te by `ns_ratio` = source_ns_per_unit /
    /// target_ns_per_unit (1.0 = none), applied before time_bucket. Callers
    /// resolve the trace's native unit (read_time_metric) and the target.
    View time_scale(double ns_ratio) const;
    /// group_by/agg/agg_numeric_args return an AggregatedView: the same lazy
    /// plan, but the type now advertises an aggregation, which is what unlocks
    /// the cache terminals (persist/reconstruct). A plain View cannot reach
    /// them, so a raw-event plan can never be persisted by mistake.
    AggregatedView group_by(std::vector<GroupKey> keys) const;
    AggregatedView agg(std::vector<AggSpec> specs) const;
    /// Aggregate with unified field expressions (Polars-style), additive to the
    /// AggSpec form: `.agg(F("dur").sum(), F("dur").mean(), F.any.count())`.
    /// Each named FieldAggExpr lowers to the matching AggSpec (field resolved
    /// by dotted name, top-level and args.* alike). The `F.any` wildcard maps
    /// to the numeric-args path: `.mean()` aggregates every numeric args.*
    /// field as a per-group mean and `.count()` is the group count; the
    /// engine's wildcard path computes only the mean, so any other
    /// `F.any.<op>()` throws INVALID_ARGUMENT.
    AggregatedView agg(std::vector<FieldAggExpr> exprs) const;
    template <class... Aggs,
              class = std::enable_if_t<
                  (... && std::is_same_v<std::decay_t<Aggs>, FieldAggExpr>)>>
    AggregatedView agg(Aggs&&... exprs) const;
    /// Aggregate every numeric args.* field as a per-group mean, one value
    /// column per discovered field (metadata and pre-aggregated *_sum/_min/_max
    /// args are skipped). Lets counters aggregate without naming the fields up
    /// front.
    AggregatedView agg_numeric_args() const;
    /// As above but applies each listed reduction to every discovered numeric
    /// arg, emitting one `<op>_<arg>` column per (arg, reduction). Only
    /// FieldStat-derivable ops are allowed (is_dyn_reduction); the AggSpec
    /// field is ignored. Lets counters report sum/min/max/var/... without
    /// naming keys.
    AggregatedView agg_numeric_args(std::vector<AggSpec> reductions) const;
    /// Out-of-core aggregation budget shared by collect(), export_counters(),
    /// and aggregate_partial(): cap each worker's in-memory group map at
    /// `bytes`, spilling to sorted temp runs that are k-way merged at the end,
    /// so the scan/merge peak stays bounded no matter the group cardinality.
    /// 0 = pure in-memory. Note collect() still materializes the final table,
    /// so this bounds intermediate memory, not the returned result.
    View memory_budget(std::uint64_t bytes) const;
    /// Convenience for memory_budget(): spill at ~1/3 of the machine's
    /// available memory, so aggregation stays bounded without a hand-picked
    /// number.
    View auto_spill() const;
    View select(std::vector<std::string> cols) const;
    /// Pagination: cap the output to `n` rows/events (0 = unlimited) after
    /// skipping `offset()` leading ones. On collect() this slices the result
    /// rows; on export it caps the streamed events.
    View limit(std::uint64_t n) const;
    View offset(std::uint64_t n) const;
    /// Post-aggregation ordering of collect()'s result rows (applied before
    /// offset/limit, on the vec sort kernels): sort_by orders by `column`
    /// (descending if set); topk keeps the `k` best rows by `column`. The same
    /// ops as dataframe::sort_by / dataframe::topk, so the View and
    /// materialized-DataFrame surfaces agree.
    View sort_by(std::string column, bool descending = false) const;
    View topk(std::string column, std::int64_t k, bool largest = true) const;
    /// Opt in to persisting this query's result as a materialized view so a
    /// later matching query reads it back instead of rescanning. A performance
    /// lever for repeated, stable query shapes (e.g. dfanalyzer); a plain
    /// collect()/read never persists. For a row query, run() writes a filtered
    /// trace split into `part_size`-byte files, each indexed at
    /// `checkpoint_size` granularity (0 = engine defaults); for an aggregation
    /// it persists a rollup.
    View materialize(std::uint64_t checkpoint_size = 0,
                     std::uint64_t part_size = 0) const;
    /// Drop `ph="M"` metadata from streamed output (default: keep).
    View metadata(bool include) const;
    /// Harvest every hash-metadata (FH/HH/SH) record even when no scanned event
    /// references it (default: off). For whole-trace metadata collection.
    View emit_all_metadata(bool v) const;
    /// Override the root directory for the aggregation cache (persist/
    /// reconstruct); empty derives it from the files' index location.
    View rollup_root(std::string dir) const;
    /// Override the root directory for materialized filtered-trace views; empty
    /// derives `<parent-of-index>/.dftindex-views`.
    View views_root(std::string dir) const;
    /// Cooperative cancellation: every terminal polls `pred` at its scan/plan
    /// loop boundaries and bails out when it returns true. Set once here rather
    /// than per terminal. The predicate owns its own semantics (e.g. a server
    /// request token that also probes the client socket).
    View cancel_when(std::function<bool()> pred) const;

    /// True when export_json() / collect() on this view would take the raw-gzip
    /// bootstrap: answer the query and build the index in one pass over an
    /// unindexed first-touch input. A caller that would otherwise pre-build the
    /// index eagerly can skip that when this is true and let the bootstrap do
    /// it. Applies to the plain export_json / collect() paths respectively.
    bool export_would_bootstrap() const;
    bool collect_would_bootstrap() const;

    /// Stream matching events to `sink` as newline-delimited JSON; the emitted
    /// lines are verbatim trace events, so a file/gzip sink yields a normal
    /// re-indexable dftracer trace.
    coro::CoroTask<ExportStats> export_json(ExportSink& sink) const;

    /// Write matching events out as a new multi-member trace via the parallel
    /// writer, choosing striped/sharded/padded layout for the target
    /// filesystem. Each gzip member holds whole lines (self-contained), so
    /// members are valid at any offset and the result re-indexes normally. This
    /// is the PFS-aware path behind `dftracer_view --merge`/`--compress`.
    coro::CoroTask<ExportStats> export_trace(TraceWriteOptions opts) const;

    /// The trace self-description (one entry per process, from each file's
    /// "end" event). Probes each file's last gzip member first (no decode of
    /// the rest); for a file whose end is scattered elsewhere (reorganized
    /// trace), falls back to a bloom-pruned scan when the file is indexed.
    /// Ignores the view's filters/agg.
    std::vector<TraceConfig> config() const;

    /// A column of the trace schema and its type, as reported by schema().
    struct ColumnInfo {
        std::string name;  ///< Dotted leaf path (e.g. "hostname", "pos.x").
        std::string type;  ///< "int64", "float64", or "string".
    };

    /// The distinct columns discoverable from this view's index: the base axis
    /// fields (pid/tid/ts/dur) plus every scalar leaf harvested at index build
    /// (top-level fields, and flat and nested args as dotted paths), unioned
    /// across the view's index files, plus a resolved.* alias for each hash
    /// column present. Schemaless: a nested-object arg surfaces as its dotted
    /// leaf columns. Reads index metadata only - no trace scan - and reads the
    /// per-index metadata in parallel. Sorted, de-duplicated. Empty for files
    /// without an index. Ignores the view's filters/agg.
    std::vector<std::string> columns() const;

    /// As columns(), each name paired with its type. Types fold across files
    /// (numeric widens to float64, any mix with a string widens to string); a
    /// column from a pre-v12 index that stored no type reads as "string".
    std::vector<ColumnInfo> schema() const;

    /// As schema(), keyed by name with the full dataframe::TypeId rather than
    /// schema()'s type-name string. Used internally so a flattened
    /// "args.<key>" column can report its concrete harvested type instead of
    /// TypeId::Unknown.
    std::unordered_map<std::string, dataframe::TypeId> column_types() const;

    /// The trace's native time unit, from the CM metadata record: a head-read
    /// of the first file only, no scan. US when the view has no files or the
    /// first file carries no CM record.
    TimeMetric time_metric() const;

    /// Run group_by + agg, returning a LazyFrame over the deferred scan. No
    /// agg counts per group; no group_by folds the whole set into one row.
    /// Builds the plan only; the scan runs on the LazyFrame's collect().
    dftracer::utils::dataframe::LazyFrame collect() const;

    /// Streaming terminal over collect()'s LazyFrame: each yielded DataFrame
    /// is a standalone chunk of at most `morsel_rows` rows (<= 0 means auto).
    /// One-shot; re-call to restart. Drains this to get collect()'s result.
    coro::AsyncGenerator<dftracer::utils::dataframe::DataFrame> stream(
        std::int64_t morsel_rows = 0) const;

    /// The eager scan behind collect(). Public for ViewSource; prefer
    /// collect() otherwise.
    coro::CoroTask<dftracer::utils::dataframe::DataFrame> collect_frame() const;

    /// True when collect() answers a raw-event row query (no group_by/agg),
    /// as opposed to an aggregation. Public for ViewSource.
    bool is_row_query() const;

    /// Containment terminals over one scan. `partition` names the lane keys;
    /// `ts`/`dur`/`name` name the interval and label fields (any field - a POD
    /// scalar reads natively, an arg/nested field is captured). call_tree
    /// returns the events plus level/parent_id; flamegraph returns the folded
    /// node frame (node_id, parent, name, level, total, self, count).
    ///
    /// `group` (flamegraph/containment/flamegraph_partial only) roots the
    /// folded tree by an arbitrary key over the raw events - any field(s), e.g.
    /// `{"pid"}`, `{"cat"}`, `{"hostname","pid"}` - so each group value gets
    /// its own top-level subtree named by that value. It is distinct from the
    /// aggregation `group_by`, which would collapse the events the tree needs.
    /// Empty (the default) folds every lane together under one root.
    coro::CoroTask<dftracer::utils::dataframe::DataFrame> call_tree(
        std::vector<std::string> partition = {"pid", "tid"},
        std::string ts = "ts", std::string dur = "dur",
        std::string name = "name") const;
    coro::CoroTask<dftracer::utils::dataframe::DataFrame> flamegraph(
        std::vector<std::string> partition = {"pid", "tid"},
        std::string ts = "ts", std::string dur = "dur",
        std::string name = "name", std::vector<std::string> group = {}) const;

    /// Both containment frames (.first = call_tree, .second = flamegraph) from
    /// one scan and one buffered fold.
    coro::CoroTask<std::pair<dftracer::utils::dataframe::DataFrame,
                             dftracer::utils::dataframe::DataFrame>>
    containment(std::vector<std::string> partition = {"pid", "tid"},
                std::string ts = "ts", std::string dur = "dur",
                std::string name = "name",
                std::vector<std::string> group = {}) const;

    /// Distributed flamegraph. A rank folds its files into an arena and
    /// serializes it; rank 0 (or a Dask reducer) passes every rank's blob to
    /// merge_flamegraph_partials for the final node frame. Partition by pid so
    /// a lane lives on one rank. `group` roots the tree as in flamegraph().
    coro::CoroTask<std::string> flamegraph_partial(
        std::vector<std::string> partition = {"pid", "tid"},
        std::string ts = "ts", std::string dur = "dur",
        std::string name = "name", std::vector<std::string> group = {}) const;
    static dftracer::utils::dataframe::DataFrame merge_flamegraph_partials(
        const std::vector<std::string_view>& partials);

    /// Build-only terminal: run the query for its side effect - materialize the
    /// rollup - and return scan stats, with no result DataFrame. The prewarm
    /// form of materialize(); idempotent when the view is already materialized.
    coro::CoroTask<ExportStats> run(const ProgressFn* progress = nullptr) const;

    /// Distributed row-MV materialize. The coordinator (a View over the FULL
    /// file set) calls materialize_dir() to create and get the shared MV
    /// directory; each rank exports its files' filtered events into a subdir of
    /// it (export_trace with build_index, e.g. `<dir>/shard-<r>/part.pfw.gz`);
    /// then the coordinator calls register_materialized(dir) to write the
    /// manifest describing the full base set. A later matching read gathers the
    /// parts across all subdirs. Empty dir when there is no views anchor.
    std::string materialize_dir() const;
    void register_materialized(const std::string& dir) const;

    /// Observability: the materialized-view trace file(s) that would serve this
    /// query (a subsuming MV), or empty if a read would scan the base. Lets a
    /// caller report or assert MV reuse without running the query.
    std::vector<std::string> mv_source() const;

    /// Read the aggregation index's regular/aggregated/counters families in one
    /// pass over shard range [shard_begin, shard_end). Distributed callers fan
    /// disjoint ranges across workers and concatenate; counters are read only
    /// when the range starts at shard 0. Empty with no aggregation tier.
    coro::CoroTask<TypedResult> collect_typed(
        int shard_begin = 0, int shard_end = 4096,
        const ProgressFn* progress = nullptr) const;

    /// Aggregate (as collect()) and emit each row as a dftracer ph="C" counter
    /// event. With a time_bucket this is the aggregator's counter-trace format,
    /// so the output is a normal re-indexable counter trace.
    coro::CoroTask<ExportStats> export_counters(ExportSink& sink) const;

    /// Distributed aggregation. Each rank aggregates its shard of files with
    /// the view's group_by/agg and returns an opaque serialized partial;
    /// combine partials from all ranks with merge_partials_to_table()
    /// (materialized collect) or merge_counter_partials() (streamed ph="C"
    /// counters). The transport between ranks (MPI etc.) is the caller's - the
    /// View core carries no MPI dependency.
    coro::CoroTask<std::string> aggregate_partial() const;
    dftracer::utils::dataframe::DataFrame merge_partials_to_table(
        const std::vector<std::string_view>& partials) const;
    ExportStats merge_counter_partials(
        const std::vector<std::string_view>& partials, ExportSink& sink) const;

    /// Escape hatch for aggregations the built-in agg vocabulary cannot express
    /// (custom bucketing, representative values, side outputs). `fold` runs
    /// over each scanned batch on one of `num_slots` worker slots, mutating
    /// that slot's partial; `combine` reduces the per-slot partials into one.
    /// The fold keeps its own logic and runs over the view's index-pruned
    /// parallel scan, so a caller consolidates onto View without forcing its
    /// shape into group_by/agg. `P` must be default-constructible and a default
    /// `P` must be the identity for `combine` (unused slots fold nothing).
    /// `limit` (0 = unlimited) caps produced events; the result's
    /// stats.truncated reports it.
    template <class P>
    coro::CoroTask<BatchResult<P>> map_batches(
        std::function<void(P&, const std::vector<std::string_view>&)> fold,
        std::function<P(P&&, P&&)> combine, std::size_t num_slots,
        std::uint64_t limit = 0) const {
        if (num_slots == 0) num_slots = 1;
        std::vector<P> partials(num_slots);
        ExportStats stats = co_await for_each_batch(
            [&](std::size_t slot, const std::vector<std::string_view>& events) {
                fold(partials[slot], events);
            },
            num_slots, limit);
        P acc = std::move(partials[0]);
        for (std::size_t i = 1; i < partials.size(); ++i)
            acc = combine(std::move(acc), std::move(partials[i]));
        co_return BatchResult<P>{std::move(acc), stats};
    }

    /// Lowest-level scan terminal: invoke `on_batch(slot, events)` for each
    /// decoded batch on worker slots in [0, num_slots), stopping at `limit`
    /// (0 = unlimited). The caller owns all per-slot state and merges after -
    /// use this when a fold keeps a big shared/per-slot accumulator that does
    /// not fit map_batches' per-slot partial + combine model. map_batches and
    /// the session build on this.
    coro::CoroTask<ExportStats> for_each_batch(
        std::function<void(std::size_t, const std::vector<std::string_view>&)>
            on_batch,
        std::size_t num_slots, std::uint64_t limit = 0) const;

    /// Open a read session over this view (see ViewSession). Parallelism is the
    /// runtime's; cap events with .limit() on the view.
    ViewSession session() const;

    /// Run caller-owned folds over one fused scan, sharing `intern` so their
    /// ids agree and per-worker slices merge. The seam behind the plugin host.
    /// `dyn_prune` (optional) is the narrowing state behind
    /// dataframe::Cursor::narrow(); a fold has no reason to pass one itself -
    /// it is for a scan-owning Cursor (ViewSource's streaming cursor) that
    /// wants a later narrow() call to prune units this scan has not claimed
    /// yet.
    coro::CoroTask<ExportStats> run_folds(
        std::span<detail::Fold* const> folds,
        dftracer::utils::StringIntern& intern,
        detail::DynamicPrune* dyn_prune = nullptr) const;

    /// The built plan. Exposes the internal representation so a caller can
    /// drive the dataframe-engine collection paths directly
    /// (see view_executor.h / view_agg_engine.h); not otherwise needed.
    const detail::ViewPlan& plan() const { return *plan_; }

   protected:
    /// Rollup terminals, reachable only through AggregatedView (which promotes
    /// them to public). materialize_partials builds the rollup from distributed
    /// aggregate_partial output (each rank scans its shard, the coordinator
    /// reduces and writes); reconstruct_if_cached returns a subsuming rollup as
    /// a table, or nullopt on a miss, so a distributed caller picks read vs
    /// recompute without a scan.
    coro::CoroTask<void> materialize_partials(
        const std::vector<std::string_view>& partials) const;
    std::optional<dftracer::utils::dataframe::DataFrame> reconstruct_if_cached()
        const;

    std::shared_ptr<const detail::ViewPlan> plan_;

   private:
    friend class ViewSession;
    friend class ViewSource;
    friend coro::CoroTask<dftracer::utils::dataframe::DataFrame>
    detail::run_collect_via_engine(const detail::ViewPlan& plan);
    friend coro::CoroTask<detail::EnginePrep> detail::prepare_engine_group(
        const detail::ViewPlan& plan);
    explicit View(std::shared_ptr<const detail::ViewPlan> plan);
};

/// A View whose type advertises a group_by/agg. Structurally identical to View
/// (same plan, same terminals), but it additionally exposes the distributed
/// rollup terminals materialize_partials()/reconstruct_if_cached(). Reached
/// only via View::group_by/agg/agg_numeric_args so a raw-event plan can never
/// call them. Builder ops are re-exposed to return AggregatedView, keeping the
/// type through a chain. A plain collect() (inherited) already reads a
/// subsuming rollup when one exists, so there is no separate cache flag:
/// materialize() builds, collect() reads.
class AggregatedView : public View {
   public:
    using View::materialize_partials;
    using View::reconstruct_if_cached;

    /// Aggregate both sides and equi-join their result DataFrames on the shared
    /// group key; the result is an empty DataFrame when the key schemas differ.
    coro::CoroTask<dftracer::utils::dataframe::DataFrame> join(
        const AggregatedView& other, JoinType how) const;

    AggregatedView filter(Query q) const {
        return {View::filter(std::move(q))};
    }
    AggregatedView filter(const FieldExpr& pred) const {
        return {View::filter(pred)};
    }
    AggregatedView query(const std::string& dsl) const {
        return {View::query(dsl)};
    }
    AggregatedView phase(Phase p) const { return {View::phase(p)}; }
    AggregatedView time_range(double begin, double end) const {
        return {View::time_range(begin, end)};
    }
    AggregatedView time_bucket(std::uint64_t interval_us) const {
        return {View::time_bucket(interval_us)};
    }
    AggregatedView time_bucket(std::uint64_t interval_us,
                               std::uint64_t origin_us) const {
        return {View::time_bucket(interval_us, origin_us)};
    }
    AggregatedView time_bucket_min(std::uint64_t interval_us) const {
        return {View::time_bucket_min(interval_us)};
    }
    AggregatedView occ_cell(std::uint64_t cell_us) const {
        return {View::occ_cell(cell_us)};
    }
    AggregatedView time_scale(double ns_ratio) const {
        return {View::time_scale(ns_ratio)};
    }
    AggregatedView group_by(std::vector<GroupKey> keys) const {
        return View::group_by(std::move(keys));
    }
    AggregatedView agg(std::vector<AggSpec> specs) const {
        return View::agg(std::move(specs));
    }
    AggregatedView agg(std::vector<FieldAggExpr> exprs) const {
        return View::agg(std::move(exprs));
    }
    template <class... Aggs,
              class = std::enable_if_t<
                  (... && std::is_same_v<std::decay_t<Aggs>, FieldAggExpr>)>>
    AggregatedView agg(Aggs&&... exprs) const {
        return View::agg(
            std::vector<FieldAggExpr>{std::forward<Aggs>(exprs)...});
    }
    AggregatedView agg_numeric_args() const { return View::agg_numeric_args(); }
    AggregatedView agg_numeric_args(std::vector<AggSpec> reductions) const {
        return View::agg_numeric_args(std::move(reductions));
    }
    AggregatedView memory_budget(std::uint64_t bytes) const {
        return {View::memory_budget(bytes)};
    }
    AggregatedView auto_spill() const { return {View::auto_spill()}; }
    AggregatedView select(std::vector<std::string> cols) const {
        return {View::select(std::move(cols))};
    }
    AggregatedView limit(std::uint64_t n) const { return {View::limit(n)}; }
    AggregatedView offset(std::uint64_t n) const { return {View::offset(n)}; }
    AggregatedView sort_by(std::string column, bool descending = false) const {
        return {View::sort_by(std::move(column), descending)};
    }
    AggregatedView topk(std::string column, std::int64_t k,
                        bool largest = true) const {
        return {View::topk(std::move(column), k, largest)};
    }
    AggregatedView materialize(std::uint64_t checkpoint_size = 0,
                               std::uint64_t part_size = 0) const {
        return {View::materialize(checkpoint_size, part_size)};
    }
    AggregatedView metadata(bool include) const {
        return {View::metadata(include)};
    }
    AggregatedView emit_all_metadata(bool v) const {
        return {View::emit_all_metadata(v)};
    }
    AggregatedView rollup_root(std::string dir) const {
        return {View::rollup_root(std::move(dir))};
    }
    AggregatedView views_root(std::string dir) const {
        return {View::views_root(std::move(dir))};
    }
    AggregatedView cancel_when(std::function<bool()> pred) const {
        return {View::cancel_when(std::move(pred))};
    }

   private:
    friend class View;
    AggregatedView(View v) : View(std::move(v)) {}
};

template <class... Aggs, class>
AggregatedView View::agg(Aggs&&... exprs) const {
    return agg(std::vector<FieldAggExpr>{std::forward<Aggs>(exprs)...});
}

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_H
