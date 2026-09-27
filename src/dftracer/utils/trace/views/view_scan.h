#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_SCAN_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_SCAN_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The scan Source: turn a plan into pruned, member-aligned candidate chunks and
// the per-chunk scanner input. Shared by every terminal, which then feed the
// decoded batches into their sink (export / aggregate / trace / counters).
namespace dftracer::utils::trace::views::detail {

class Fold;

/// What a scan delivers, so an index fold can refuse one that cannot establish
/// its artifact. The planner never widens a scan to make an artifact buildable;
/// it just does not attach. Pruning is absent on purpose: it is decided after
/// attach, so it is a coverage fact, not a requirement.
struct ScanShape {
    bool filtered = false;
    /// Metadata records reach the folds.
    bool include_metadata = false;
};

std::size_t checkpoint_size_or_default(std::size_t s);

// What this scan delivers, for deciding which artifacts it can establish.
ScanShape scan_shape(const ViewDefinition& vdef);

// The index-building folds worth riding along on this scan (dictionary, bloom):
// those the scan can establish, and only while some file still lacks the pruner
// bloom, so a repeat scan of an already-built index attaches nothing.
std::vector<std::unique_ptr<Fold>> select_index_folds(
    const ViewPlan& plan, const ViewDefinition& vdef,
    dftracer::utils::StringIntern& intern);

// One candidate chunk to scan: a byte range within a file's index.
struct ScanUnit {
    std::string file_path;
    std::string index_path;
    std::size_t checkpoint_size = 0;
    std::uint64_t checkpoint_idx = 0;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
    // Place in the scan's unit list: file order, then checkpoint order.
    std::size_t seq = 0;
    // The View query with resolved columns rewritten for this unit's index;
    // null when there was nothing to rewrite.
    std::shared_ptr<const duql::Query> query;
};

// The record_schema every file of `plan` was indexed under, or is detected as
// when unindexed; a file without records (empty, or only metadata) takes no
// vote. Throws INVALID_ARGUMENT when the record_schemas of files with records
// differ.
const dftracer::utils::index::RecordSchema& plan_record_schema(
    const ViewPlan& plan);

// Whether the plan's records are decoded by exact JSON path (a record_schema
// other than dftracer's), so columns and fields are named by path.
bool plan_by_path(const ViewPlan& plan);

// The trace roles an operation reads. DURATION implies TIME.
enum class TraceRole : std::uint8_t { TIME, DURATION };

// Throws INVALID_ARGUMENT naming `op` when the files' record_schema binds no
// path to `role` (a time role for TIME; time and duration for DURATION).
void require_role(const ViewPlan& plan, std::string_view op,
                  TraceRole role = TraceRole::TIME);

// Fold the phase selector into the query: Events -> ph=="X", Counters ->
// ph=="C", Any -> no constraint. ANDed with any user filter and, for a plan
// with a build step, the key sets that step gives, which runs it: call it
// only when executing the plan.
std::optional<duql::Query> effective_query(const ViewPlan& plan);

// Whether `plan`'s rows include metadata records: phase("metadata") or
// duql's `all`.
bool metadata_rows(const ViewPlan& plan);

// Build the scanner's ViewDefinition from a plan. `for_aggregation` drops
// ph="M" metadata (aggregation ignores it); otherwise metadata follows the
// plan's include/emit flags. Every terminal starts from one of these two
// shapes.
ViewDefinition make_vdef(const ViewPlan& plan, bool for_aggregation);

bool is_cancelled(const ViewPlan& plan);

// `q` applies unless the unit carries its rewritten query.
ViewScannerInput make_scanner_input(const ScanUnit& u,
                                    const ViewDefinition& vdef,
                                    const std::optional<duql::Query>& q);

// Plan every file in parallel (metadata + prune) and flatten the surviving
// candidates into a single work list; `skipped_out` gets the pruned-chunk
// count. Resolved columns are rewritten once per index before planning, and
// each unit carries the rewritten query. Throws if an index cannot be read
// for that rewrite.
coro::CoroTask<std::vector<ScanUnit>> gather_units(const ViewPlan& plan,
                                                   const ViewDefinition& vdef,
                                                   std::uint64_t& skipped_out);

// The shared scan driver: gather candidate chunks and scan them in parallel,
// handing each decoded batch to `on_batch(slot, events)`. `num_slots` worker
// coroutines round-robin the units (0 = one worker per unit); `limit`
// (0 = unlimited) caps total produced events. Every per-slot body a streaming
// terminal needs (write to a sink, fold into a slot map, ...) is the callback;
// the gather + fan-out + per-chunk scan loop lives here once.
// `folds` (optional) ride along: each matched batch is parsed once into owned
// FoldEvents (via `intern`, required when folds are present) and fanned to
// every fold, which is told which units were read end to end and finalized once
// the fan-out joins. They can only ever slow the consumer by what they do per
// batch, never by waiting. Returns artifacts_committed set if any fold wrote.
coro::CoroTask<ExportStats> for_each_scanned_batch(
    const ViewPlan& plan, const ViewDefinition& vdef, std::size_t num_slots,
    std::uint64_t limit,
    const std::function<void(std::size_t,
                             const std::vector<std::string_view>&)>& on_batch,
    std::span<Fold* const> folds = {},
    dftracer::utils::StringIntern* intern = nullptr);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_SCAN_H
