#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_EXECUTOR_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_EXECUTOR_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <functional>
#include <memory>
#include <span>

namespace dftracer::utils {
class StringIntern;
}

namespace dftracer::utils::trace::views::detail {

class Fold;
class DynamicPrune;

// True when a first-touch query would take the raw-gzip bootstrap (answer the
// query and build the index in one pass); the exact gate the export/collect
// terminals apply internally, exposed so the CLI does not duplicate them.
bool export_bootstrap_eligible(const ViewPlan& plan);
bool collect_bootstrap_eligible(const ViewPlan& plan);

coro::CoroTask<ExportStats> run_export(const ViewPlan& plan, ExportSink& sink);

// Write matching events as a new multi-member trace via the parallel writer
// (PFS-aware); sharded output is concatenated on close.
coro::CoroTask<ExportStats> run_export_trace(const ViewPlan& plan,
                                             const TraceWriteOptions& opts);

// Fused variant of run_export_trace that builds the member+bloom+stats index
// during the write, avoiding a re-inflate. A single in-order consumer writes
// members so member_idx == checkpoint_idx and no offset remap is needed.
// Reached when TraceWriteOptions::build_index is set.
coro::CoroTask<ExportStats> run_export_trace_indexed(
    const ViewPlan& plan, const TraceWriteOptions& opts,
    const ProgressFn* progress = nullptr);

/// Resolve a min-aligned bucket origin (plan.bucket_origin_min) to the trace's
/// minimum timestamp, read from the index zone maps (no event scan). A no-op
/// (returns `plan` unchanged) when min alignment or bucketing is not
/// requested, so callers can apply it defensively.
ViewPlan resolve_bucket_origin(const ViewPlan& plan);

/// The no-scan fast paths, in order: the first-touch raw-gzip bootstrap, then
/// the aggregation tier. On a hit fills `out` (a mergeable engine AggState,
/// finalize with finalize_engine_result) and returns true; false means the
/// query must scan. The AggState rollup is served separately by
/// try_serve_rollup (it returns a finalized DataFrame).
coro::CoroTask<bool> try_serve_aggregate_no_scan(
    const ViewPlan& plan, dftracer::utils::dataframe::AggStatePtr& out);

/// Serve `plan` from a subsuming persisted rollup with no scan: re-aggregate
/// the stored AggState partials to `plan`'s grouping and finalize. nullopt when
/// no rollup subsumes it (the query must scan or use another fast path).
std::optional<dftracer::utils::dataframe::DataFrame> try_serve_rollup(
    const ViewPlan& plan);

/// True for a row query (no group_by / agg / numeric-args): collect() returns
/// the matching events, not an aggregate.
bool is_row_query(const ViewPlan& plan);

/// Row-query collect: scan the matching events into one native DataFrame (via
/// NativeRowFold), applying select/sort/topk/offset/limit. Assumes
/// is_row_query(plan).
coro::CoroTask<dataframe::DataFrame> run_collect_rows(const ViewPlan& plan);

/// Scan, buffer the lean containment tuples in a ContainmentFold, then build
/// the call-tree / flamegraph frame. `partition` names the lane keys;
/// ts/dur/name name the interval and label fields (any field, POD scalar or
/// arg/nested).
coro::CoroTask<dataframe::DataFrame> run_call_tree(
    const ViewPlan& plan, std::vector<std::string> partition,
    std::string ts_field, std::string dur_field, std::string name_field);
coro::CoroTask<dataframe::DataFrame> run_flamegraph(
    const ViewPlan& plan, std::vector<std::string> partition,
    std::string ts_field, std::string dur_field, std::string name_field,
    std::vector<std::string> group = {});
coro::CoroTask<std::pair<dataframe::DataFrame, dataframe::DataFrame>>
run_containment(const ViewPlan& plan, std::vector<std::string> partition,
                std::string ts_field, std::string dur_field,
                std::string name_field, std::vector<std::string> group = {});
coro::CoroTask<std::string> run_flamegraph_partial(
    const ViewPlan& plan, std::vector<std::string> partition,
    std::string ts_field, std::string dur_field, std::string name_field,
    std::vector<std::string> group = {});

// Run caller-owned `folds` as one fused scan of `plan`, sharing `intern` so
// their ids agree and per-worker slices merge. `dyn_prune` (optional) is
// forwarded to fuse() so a cursor's narrow() can still prune units not yet
// claimed.
coro::CoroTask<ExportStats> run_folds(const ViewPlan& plan,
                                      std::span<Fold* const> folds,
                                      dftracer::utils::StringIntern& intern,
                                      DynamicPrune* dyn_prune = nullptr);

// Build-only terminal: materialize the filtered-trace MV (row query) or the
// rollup (aggregation) and return scan stats. A no-op when the MV already
// exists, so it is an idempotent prewarm. `progress` gets (done, total) units.
coro::CoroTask<ExportStats> run_materialize(
    const ViewPlan& plan, const ProgressFn* progress = nullptr);

// Distributed materialize: reduce rank-local partials (from aggregate_partial)
// app-side and persist the rollup, without re-scanning.
coro::CoroTask<void> run_materialize_partials(
    const ViewPlan& plan, const std::vector<std::string_view>& partials);

// Read the aggregation index's three record families (regular events,
// aggregated records, counters) from shard range [shard_begin, shard_end).
// Empty tables when the index has no aggregation tier.
coro::CoroTask<TypedResult> run_collect_typed(
    const ViewPlan& plan, int shard_begin = 0, int shard_end = 4096,
    const ProgressFn* progress = nullptr);

// Return a subsuming rollup as a table, or nullopt on a miss, so a distributed
// caller decides read vs recompute without a scan.
std::optional<dataframe::DataFrame> run_reconstruct_if_cached(
    const ViewPlan& plan);

// Streaming, out-of-core counter aggregation under a per-worker memory budget
// (plan.memory_budget): spill to sorted runs, k-way merge, and emit each group
// as a ph="C" event. Peak memory is bounded; budget 0 stays purely in-memory.
coro::CoroTask<ExportStats> run_export_counters(const ViewPlan& plan,
                                                ExportSink& sink);

// Distributed building blocks: rank-local aggregate -> opaque serialized
// partial, and merge partials -> emit. The MPI (or other) transport lives in
// the caller so the View core stays transport-free.
coro::CoroTask<std::string> run_aggregate_partial(const ViewPlan& plan);
ExportStats merge_counters_partials(
    const ViewPlan& plan, const std::vector<std::string_view>& partials,
    ExportSink& sink);
dataframe::DataFrame merge_partials_to_table(
    const ViewPlan& plan, const std::vector<std::string_view>& partials);

// Drive the plan's index-pruned parallel scan; invoke `on_batch(slot, events)`
// per decoded batch on worker slots in [0, num_slots), stopping at `limit`
// (0 = unlimited).
coro::CoroTask<ExportStats> run_scan_batches(
    const std::shared_ptr<const ViewPlan>& plan, std::size_t num_slots,
    std::uint64_t limit,
    const std::function<void(std::size_t,
                             const std::vector<std::string_view>&)>& on_batch);

// The fused-session engine behind ViewSession; internal to the executor.
struct ViewSessionState;

// A cache-hittable aggregation branch: its group_by/agg complete the base plan
// into a full plan whose rollup run_session tries to reconstruct before
// scanning. A hit fills `out` and the branch skips the scan.
struct AggBranch {
    std::vector<GroupKey> group_by;
    std::vector<AggSpec> agg;
    std::shared_ptr<dataframe::DataFrame> out;
    // The branch's full plan (collect(const View&)), used directly instead of
    // overlaying group_by/agg on the base. apply_query filters per event on the
    // shared scan, keeping the branch off the no-scan rollup/tier path.
    std::shared_ptr<const ViewPlan> plan;
    bool apply_query = false;
    // When set, emits the serialized (raw-keyed) aggregate partial into
    // `partial_out` for a distributed merge instead of a DataFrame into `out`.
    // Forces a scan (raw partial, no rollup relabel).
    std::shared_ptr<std::string> partial_out;
    // A predicated collect (no branch plan): the per-event filter overlaid onto
    // the base plan, applied by the branch's engine fold (apply_query).
    std::optional<query::Query> query;
};

// One branch of a fused session: a predicate selecting events, a per-event
// `consume` (slot, parsed event, raw JSON), and a `finalize` that reduces the
// branch's per-slot partials into its result. `agg` is set only for a match-all
// aggregation collect branch, enabling the pre-scan rollup reconstruct.
/// One raw branch of a fused session. `make_consumer` is called once per fuse
/// worker, serially, before the parallel scan starts, and the consumer it
/// returns is never shared between workers, so it needs no locking. `finalize`
/// runs once after the scan and reduces whatever state those consumers hold.
/// A branch's per-worker consumer: the parsed event plus its raw JSON bytes.
using BranchConsumer =
    std::function<void(const json::JsonValue&, std::string_view)>;

struct BranchHooks {
    std::optional<query::Query> predicate;  // nullopt = match all
    std::function<BranchConsumer()> make_consumer;
    std::function<void()> finalize;
    std::optional<AggBranch> agg;
};

void add_fold_branch(ViewSessionState& state, Query predicate,
                     std::function<BranchConsumer()> make_consumer,
                     std::function<void()> finalize);

// Match-all fold branch (no per-branch predicate): consume every scanned event.
void add_fold_branch(ViewSessionState& state,
                     std::function<BranchConsumer()> make_consumer,
                     std::function<void()> finalize);

// Attach a match-all branch that aggregates (group_by + agg) and, on finalize,
// persists the result as a rollup under the session's base plan overlaid with
// this branch's group_by/agg. Build-only (no Batch), so one scan materializes
// several rollups; a later matching collect()/reconstruct reads them back.
void add_materialize_branch(ViewSessionState& state,
                            std::vector<GroupKey> group_by,
                            std::vector<AggSpec> agg);
std::shared_ptr<ViewSessionState> make_view_session_state(
    std::shared_ptr<const ViewPlan> plan);
void add_branch(ViewSessionState& state, BranchHooks hooks);

/// Offer `q` as a narrowing of the session's shared scan. execute() applies it
/// only when no other branch is registered; see ViewSessionState.
void propose_base_prune(ViewSessionState& state, query::Query q);

// Attach an externally-built Fold to the session's shared scan: `make`
// constructs it with the scan's intern, `finalize` runs after the merge.
void add_fold_factory(
    ViewSessionState& state,
    std::function<std::unique_ptr<Fold>(dftracer::utils::StringIntern&)> make,
    std::function<void()> finalize);
coro::CoroTask<ExportStats> run_session(
    std::shared_ptr<ViewSessionState> state);

// Built-in branch terminals; the caller sets `predicate` on the returned hooks.
BranchHooks make_export_branch(ExportSink& sink,
                               std::shared_ptr<ExportStats> out);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_EXECUTOR_H
