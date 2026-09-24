#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_H

#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/field.h>
#include <dftracer/utils/dataframe/lazy_ops.h>
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
        Field,
        /// A column the plan computes per event from other fields; `arg`
        /// names it. Its values keep their type.
        Expr
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

/// Options for sink_trace(): write the view's events as a new
/// re-indexable multi-member trace through the parallel (PFS-aware) writer.
/// `member_size` is the target uncompressed bytes per gzip member (0 =
/// default); `num_workers` caps the writer fan-out. The writer layout
/// (striped/sharded/padded) and the exact worker count are chosen from the
/// target filesystem.
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
    /// Called once an export into this sink has written everything, so a
    /// buffering sink can make the output visible. A sink may be written by
    /// several exports, so this is not an end of stream.
    virtual void flush() {}
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
/// templates below build on are private members of View/ViewSession,
/// defined in the .cpp - no engine surface is exposed in this public header.
struct ViewPlan;
/// A trace scan's immutable plan, shared by the View, its source and the
/// session that runs it. Internal: users hold a View.
using ScanPlan = std::shared_ptr<const ViewPlan>;
class Fold;
class DynamicPrune;
struct ViewSessionState;

/// Phase 1 of the scan -> dataframe engine aggregation convergence
/// (view_agg_engine.h/.cpp); builds the
/// raw row-query LazyFrame the streaming group_by runs over.
coro::CoroTask<dftracer::utils::dataframe::DataFrame> run_collect_via_engine(
    const ViewPlan& plan);

// Builds the engine group-by inputs (and the raw scan plan) for a plan.
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

class ViewSource;

/// The two handles a single containment branch yields from one buffered fold.
struct ContainmentHandles {
    Deferred<dftracer::utils::dataframe::DataFrame> call_tree;
    Deferred<dftracer::utils::dataframe::DataFrame> flamegraph;
};

/// A batch of read ops over one shared scan of a base plan. Register ops
/// (collect/materialize/fold/export_json) - each returns a Deferred handle -
/// then execute() runs them together, serving a materialized aggregate from its
/// rollup and scanning only the rest. Unlike View::collect(), which runs
/// at once, the ops here resolve only on execute().
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

    /// Persist the (group_by + agg) aggregation over ALL scanned events as a
    /// rollup, sharing the one scan. Build-only (no handle); a later matching
    /// collect() reads it back.
    void materialize(std::vector<GroupKey> group_by, std::vector<AggSpec> agg);

    /// Stream the branch's matching events verbatim to `sink`.
    Deferred<ExportStats> export_json(Query predicate, ExportSink& sink);

    /// Stream every scanned event to `sink`, sharing the session's scan.
    Deferred<ExportStats> export_json(ExportSink& sink);

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
    friend class ViewSource;
    explicit ViewSession(detail::ScanPlan plan);

    // Branches over a plan built off this session's base; View and
    // ViewSource register them.
    /// Register an aggregation view as a branch, carrying its full plan.
    /// `branch` must be built off this session's base view.
    Deferred<dftracer::utils::dataframe::DataFrame> collect(
        const detail::ScanPlan& branch);

    /// Aggregate `branch` into a serialized partial (opaque bytes) for a
    /// distributed merge, over the shared scan. Merge and resolve later.
    Deferred<std::string> aggregate_partial(const detail::ScanPlan& branch);

    /// Materialize the branch's matching events into a DataFrame over the
    /// shared scan. The whole matching set is held in memory, so filter to
    /// bound it.
    Deferred<dftracer::utils::dataframe::DataFrame> collect_events(
        const detail::ScanPlan& branch);

    /// Containment over the shared scan: the branch's events buffer into a lean
    /// per-event tuple, and the parallel per-lane nesting pass runs on execute.
    /// call_tree returns the events plus level/parent_id; flamegraph returns
    /// the folded node frame. `partition`/`ts`/`dur`/`name` are named fields
    /// (POD scalar fast path, else captured arg/nested).
    Deferred<dftracer::utils::dataframe::DataFrame> call_tree(
        const detail::ScanPlan& branch,
        std::vector<std::string> partition = {"pid", "tid"},
        std::string ts = "ts", std::string dur = "dur",
        std::string name = "name");

    Deferred<dftracer::utils::dataframe::DataFrame> flamegraph(
        const detail::ScanPlan& branch,
        std::vector<std::string> partition = {"pid", "tid"},
        std::string ts = "ts", std::string dur = "dur",
        std::string name = "name");

    /// Both containment outputs from ONE buffered fold over the shared scan -
    /// the events buffer once and each lane sorts once, feeding both builders.
    ContainmentHandles containment(const detail::ScanPlan& branch,
                                   std::vector<std::string> partition = {"pid",
                                                                         "tid"},
                                   std::string ts = "ts",
                                   std::string dur = "dur",
                                   std::string name = "name");

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
        const detail::ScanPlan& branch,
        const std::vector<std::string>& partition, const std::string& ts,
        const std::string& dur, const std::string& name,
        const std::vector<std::string>& group,
        std::shared_ptr<dftracer::utils::dataframe::DataFrame> out_ct,
        std::shared_ptr<dftracer::utils::dataframe::DataFrame> out_fg,
        std::shared_ptr<std::string> out_partial = nullptr);
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

enum class JoinType;

/// A column of the trace schema and its type, as the index reports it.
struct ColumnInfo {
    std::string name;  ///< Dotted leaf path (e.g. "hostname", "pos.x").
    std::string type;  ///< "int64", "float64", or "string".
};

/// Collects several plans together: record them with collect(), then
/// execute() once. Plans over the same trace base share one scan, as
/// dataframe::collect_all() does; each handle resolves on execute().
class View;

/// A caller branch of a trace scan: registers itself on the session that runs
/// the scan and returns the call that publishes its result once that session
/// has executed, given the scan's stats. The branch sees every event the
/// session scans.
using SessionBranch =
    std::function<std::function<void(const ExportStats&)>(ViewSession&)>;

/// Both containment frames of one buffered fold.
struct ContainmentResult {
    dataframe::DataFrame call_tree;
    dataframe::DataFrame flamegraph;
};

/// Selects the deferred overload of a sink: it returns a LazyResult for
/// collect_all() or a session instead of running at once.
struct Lazy {};
inline constexpr Lazy LAZY{};

namespace detail {
template <class T>
coro::CoroTask<void> resolve_into(
    std::shared_ptr<T> out, typename dataframe::LazyResult<T>::Finish finish,
    std::vector<dataframe::DataFrame> frames) {
    *out = co_await finish(std::move(frames));
}
}  // namespace detail

/// Plans registered here run together on execute(), as collect_all() runs
/// them: plans over the same trace files share one scan.
class TraceSession {
   public:
    Deferred<dataframe::DataFrame> collect(dataframe::LazyFrame plan);
    template <class T>
    Deferred<T> collect(dataframe::LazyResult<T> result) {
        auto out = std::make_shared<T>();
        add(result.plans(), [out, finish = result.finish()](
                                std::vector<dataframe::DataFrame> frames) {
            return detail::resolve_into<T>(out, finish, std::move(frames));
        });
        return {out, executed_};
    }
    /// tv.sink_json(sink, LAZY) and tv.materialize(LAZY), registered.
    Deferred<ExportStats> sink_json(const View& tv, ExportSink& sink);
    Deferred<ExportStats> materialize(const View& tv);
    coro::CoroTask<void> execute();

   private:
    using Resolve =
        std::function<coro::CoroTask<void>(std::vector<dataframe::DataFrame>)>;
    struct Pending {
        std::size_t count;
        Resolve resolve;
    };
    void add(const std::vector<dataframe::LazyFrame>& plans, Resolve resolve);

    std::vector<dataframe::LazyFrame> plans_;
    std::vector<Pending> pending_;
    std::shared_ptr<bool> executed_ = std::make_shared<bool>(false);
};

/// A lazy, composable view over trace files: a LazyFrame whose source is a
/// trace scan. Generic LazyFrame builders (inherited from LazyOps) record ops
/// that the scan absorbs at plan time, so filters, projections and group-bys
/// reach the index; the trace builders below (event filters, phase, time
/// range, time buckets, group keys, trace aggregates) shape the scan itself.
/// Nothing runs until collect(), which returns the DataFrame.
///
/// A trace builder shapes the events the scan reads, so it may only follow
/// filters: calling one after any other op throws INVALID_ARGUMENT naming that
/// op. Event filters (query / Query / FieldExpr) always select raw events, even
/// after a trace group_by; an Expr filter follows LazyFrame order.
class View : public dataframe::LazyOps<View> {
   public:
    /// An empty view (no files); collects to an empty result.
    View();

    static View from_file(std::string file_path, std::string index_path = "");
    static View from_files(std::vector<ViewFile> files,
                           indexing::BloomFilterCache* bloom_cache = nullptr);
    /// Scans `dir` recursively for .pfw.gz traces, sorted by path; each
    /// file's index resolves under `index_path` (empty = beside the trace).
    static coro::CoroTask<View> from_directory(std::string dir,
                                               std::string index_path = "");

    using LazyOps<View>::filter;
    using LazyOps<View>::group_by;

    using LazyOps<View>::select;

    /// On raw events with only filters before it, the fields the scan reads:
    /// any field, indexed or not, and a bare arg name reads that arg as
    /// "args.<key>". Otherwise a projection of the plan's
    /// columns.
    View select(std::vector<std::string> names) const;
    View filter(Query q) const;
    View filter(const FieldExpr& pred) const;
    View query(const std::string& dsl) const;
    View phase(Phase p) const;
    View time_range(double begin, double end) const;
    View time_bucket(std::uint64_t interval_us) const;
    View time_bucket(std::uint64_t interval_us, std::uint64_t origin_us) const;
    View time_bucket_min(std::uint64_t interval_us) const;
    /// Grid, in microseconds, that the occupancy aggregates (busy,
    /// concurrency, utilization, active) snap interval edges to; 0 is the
    /// exact union. Honored only with a time_range.
    View resolution(std::uint64_t cell_us) const;
    View time_scale(double ns_ratio) const;
    View group_by(std::vector<GroupKey> keys) const;
    View agg(std::vector<AggSpec> specs) const;
    View agg(std::vector<FieldAggExpr> exprs) const;
    View agg_numeric_args() const;
    View agg_numeric_args(std::vector<AggSpec> reductions) const;
    View metadata(bool include) const;
    View emit_all_metadata(bool v) const;
    View rollup_root(std::string dir) const;
    View views_root(std::string dir) const;
    View cancel_when(std::function<bool()> pred) const;
    /// Spill budget for both the trace aggregation and the LazyFrame ops.
    View memory_budget(std::uint64_t bytes) const;

    /// Index metadata only; no trace event is decoded.
    std::vector<std::string> columns() const;
    std::vector<ColumnInfo> column_info() const;
    TimeMetric time_metric() const;
    /// The spill budget set on this view, in bytes (0 = the default).
    std::uint64_t memory_budget_bytes() const;

    TraceSession session() const { return {}; }

    /// True when this viewer's ops all absorb into an aggregating trace scan.
    bool aggregates() const;

    /// Containment terminals: a plan over the events this viewer selects:
    /// call_tree gives the events plus level/parent_id per lane of
    /// `partition`, flamegraph the events folded by name path. The returned
    /// plan chains like any LazyFrame; the trace methods end here. Each throws
    /// INVALID_ARGUMENT naming the first op of this viewer that the terminal
    /// cannot take (anything but absorbed filters and projections).
    dataframe::LazyFrame call_tree(std::vector<std::string> partition = {"pid",
                                                                         "tid"},
                                   std::string ts = "ts",
                                   std::string dur = "dur",
                                   std::string name = "name") const;
    dataframe::LazyFrame flamegraph(
        std::vector<std::string> partition = {"pid", "tid"},
        std::string ts = "ts", std::string dur = "dur",
        std::string name = "name", std::vector<std::string> group = {}) const;
    /// Both frames from one buffered fold.
    dataframe::LazyResult<ContainmentResult> containment(
        std::vector<std::string> partition = {"pid", "tid"},
        std::string ts = "ts", std::string dur = "dur",
        std::string name = "name", std::vector<std::string> group = {}) const;

    /// Mergeable partials for a distributed run: merge flamegraph partials
    /// with merge_flamegraph_partials(), aggregate partials with
    /// merge_partials() on a viewer of the same aggregation. A trailing head
    /// before flamegraph_partial caps the scan as in for_each_batch.
    dataframe::LazyResult<std::string> flamegraph_partial(
        std::vector<std::string> partition = {"pid", "tid"},
        std::string ts = "ts", std::string dur = "dur",
        std::string name = "name", std::vector<std::string> group = {}) const;
    dataframe::LazyResult<std::string> aggregate_partial() const;
    static dataframe::DataFrame merge_flamegraph_partials(
        const std::vector<std::string_view>& partials);
    dataframe::DataFrame merge_partials(
        const std::vector<std::string_view>& partials) const;

    /// The aggregation index's record families.
    dataframe::LazyResult<TypedResult> typed(int shard_begin = 0,
                                             int shard_end = 4096,
                                             ProgressFn progress = {}) const;
    coro::CoroTask<TypedResult> collect_typed(int shard_begin = 0,
                                              int shard_end = 4096,
                                              ProgressFn progress = {}) const;

    /// Sinks run when awaited; the LAZY overloads defer for collect_all() or
    /// a session. sink_json writes the selected events (their projection when
    /// one was absorbed) to `sink`, which must outlive the run. materialize
    /// persists this viewer's result for later reads to serve.
    coro::CoroTask<ExportStats> sink_json(ExportSink& sink) const;
    dataframe::LazyResult<ExportStats> sink_json(ExportSink& sink, Lazy) const;
    /// As sink_json(sink, LAZY), with the plan sharing ownership of `sink`.
    dataframe::LazyResult<ExportStats> sink_json(
        std::shared_ptr<ExportSink> sink, Lazy) const;
    /// Writes the selected events as a new indexed trace
    /// (multi-member, re-indexable).
    coro::CoroTask<ExportStats> sink_trace(TraceWriteOptions opts) const;
    dataframe::LazyResult<ExportStats> sink_trace(TraceWriteOptions opts,
                                                  Lazy) const;
    coro::CoroTask<ExportStats> materialize(std::uint64_t checkpoint_size = 0,
                                            std::uint64_t part_size = 0,
                                            ProgressFn progress = {}) const;
    dataframe::LazyResult<ExportStats> materialize(
        Lazy, std::uint64_t checkpoint_size = 0, std::uint64_t part_size = 0,
        ProgressFn progress = {}) const;

    /// Writes this viewer's aggregation as ph="C" counter events
    /// (a re-indexable counter trace).
    coro::CoroTask<ExportStats> sink_counters(ExportSink& sink) const;

    /// This viewer's group_by + agg applied to `variant`'s events as well,
    /// joined on the group key: keys, `l_`/`r_` per metric, then
    /// `delta_`/`pct_` (DataFrame::compare_agg). Both sides over the same
    /// files share one scan.
    dataframe::LazyFrame compare(const View& variant) const;

    /// Low-level scans of this view's events, for callers whose work does not
    /// fit a plan. for_each_batch calls `on_batch(slot, events)` with raw
    /// event lines on worker slots [0, num_slots). `limit` (0 = all), or a
    /// trailing head on this view, stops the scan between batches once that
    /// many events were delivered, so a batch may carry more.
    /// map_batches folds each batch into a per-slot partial and reduces them
    /// with `combine`; a default `P` must be the identity of combine.
    coro::CoroTask<ExportStats> for_each_batch(
        std::function<void(std::size_t, const std::vector<std::string_view>&)>
            on_batch,
        std::size_t num_slots, std::uint64_t limit = 0) const;
    template <class P>
    coro::CoroTask<BatchResult<P>> map_batches(
        std::function<void(P&, const std::vector<std::string_view>&)> fold,
        std::function<P(P&&, P&&)> combine, std::size_t num_slots,
        std::uint64_t limit = 0) const {
        if (num_slots == 0) num_slots = 1;
        std::vector<P> partials(num_slots);
        // A named callback, not a temporary in the co_await: GCC 11-12 destroy
        // such temporaries twice.
        std::function<void(std::size_t, const std::vector<std::string_view>&)>
            on_batch = [&partials, &fold](
                           std::size_t slot,
                           const std::vector<std::string_view>& events) {
                fold(partials[slot], events);
            };
        ExportStats stats =
            co_await for_each_batch(std::move(on_batch), num_slots, limit);
        P acc = std::move(partials[0]);
        for (std::size_t i = 1; i < partials.size(); ++i)
            acc = combine(std::move(acc), std::move(partials[i]));
        co_return BatchResult<P>{std::move(acc), stats};
    }
    /// Run caller-owned folds over one scan of this view's events, sharing
    /// `intern` so their ids agree and per-worker slices merge.
    coro::CoroTask<ExportStats> run_folds(
        std::span<detail::Fold* const> folds,
        dftracer::utils::StringIntern& intern) const;

    /// The trace self-description, one entry per process, from each file's
    /// "end" event (TraceConfig). Ignores this view's filters.
    std::vector<TraceConfig> config() const;
    /// True when sink_json / collect() would take the first-touch pass that
    /// answers the query and builds the index at once, so a caller that would
    /// build the index first can skip that.
    bool export_would_bootstrap() const;
    bool collect_would_bootstrap() const;
    /// Write merged aggregate_partial() output as counter events, with this
    /// view's aggregation.
    ExportStats merge_counter_partials(
        const std::vector<std::string_view>& partials, ExportSink& sink) const;

    /// A caller branch of the scan over this viewer's events, as a plan that
    /// yields no columns; `attach` runs once per collect of the plan. It
    /// shares a batch's scan only when this viewer has no filter, since the
    /// branch sees every event that scan reads. A trailing head caps the scan
    /// as in for_each_batch.
    dataframe::LazyFrame branch(SessionBranch attach) const;
    /// branch() for a branch whose result is a Deferred<T>, such as
    /// plugins::Plugins::attach. The result is read from a slot the plan owns,
    /// so collect one such result at a time.
    template <class T>
    dataframe::LazyResult<T> branch(
        std::function<Deferred<T>(ViewSession&)> attach) const {
        auto slot = std::make_shared<T>();
        dataframe::LazyFrame plan =
            branch([attach = std::move(attach), slot](ViewSession& s) {
                Deferred<T> h = attach(s);
                return std::function<void(const ExportStats&)>(
                    [h, slot](const ExportStats&) mutable {
                        *slot = std::move(h.get());
                    });
            });
        return {{std::move(plan)}, [slot](std::vector<dataframe::DataFrame>) {
                    return dataframe::detail::ready(std::move(*slot));
                }};
    }

    /// The distributed rollup terminals, over this viewer's
    /// aggregation. The bytes behind `partials` must outlive the task.
    coro::CoroTask<void> materialize_partials(
        const std::vector<std::string_view>& partials) const;
    std::optional<dataframe::DataFrame> reconstruct_if_cached() const;

    /// Materialized-view bookkeeping over this viewer's events.
    std::vector<std::string> mv_source() const;
    std::string materialize_dir() const;
    void register_materialized(const std::string& dir) const;

    /// The plan: the trace scan as a source plus the ops above it.
    const dataframe::LazyFrame& lazy() const { return lf_; }
    View with_lazy(dataframe::LazyFrame lf) const;
    /// True while a filter still selects raw events: the scan reads events
    /// and the plan holds nothing but filters. Reads no index.
    bool filters_events() const;

   private:
    // EventsWindow: events, optionally followed by one row window (head /
    // slice), which becomes the scan's offset and limit. EventsHead: the
    // same, but the window may not skip rows.
    enum class Need : std::uint8_t {
        Any,
        Events,
        EventsHead,
        EventsWindow,
        Aggregate
    };

    explicit View(detail::ScanPlan plan);
    View(detail::ScanPlan plan, dataframe::LazyFrame lf);
    View reshape(const char* builder,
                 const std::function<detail::ScanPlan(const detail::ScanPlan&)>&
                     step) const;
    /// The scan plan with every op of this view absorbed into it, for the
    /// terminal `method`; throws naming the first op that stays behind.
    detail::ScanPlan absorbed(const char* method, Need need) const;

    detail::ScanPlan plan_;
    dataframe::LazyFrame lf_;
};

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_H
