#ifndef DFTRACER_UTILS_TRACE_GENESIS_GENESIS_H
#define DFTRACER_UTILS_TRACE_GENESIS_GENESIS_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::trace::genesis {

inline constexpr double SKETCH_ACCURACY = 0.01;

struct RunKeys {
    std::string app;
    std::string system;
    std::string unique_input;
    std::int64_t nodes = 0;
    std::int64_t ppn = 0;
    std::string papi_set;
};

/// One PAPI set of a run group. `papi_counters` (sorted) identifies the set's
/// processes when a group holds several sets; empty for single-set groups.
struct SetSpec {
    RunKeys keys;
    std::vector<std::string> papi_counters;
    std::vector<std::string> gpu_csvs;
};

/// Files read together: one set (matrix) or all sets of a tioga run
/// directory, whose time slices mix sets. With `sets_from_counters`, `sets`
/// holds one template whose keys lack `papi_set`, and each distinct PAPI
/// counter list found in the traces becomes a set.
struct RunGroup {
    std::string dir;
    std::string rel_dir;
    std::vector<std::string> files;
    std::uint64_t compressed_bytes = 0;
    std::string summary_json;
    std::vector<SetSpec> sets;
    bool sets_from_counters = false;
};

struct Skip {
    std::string dir;
    std::string file;
    std::string reason;
};

struct Discovery {
    std::vector<RunGroup> groups;
    std::vector<Skip> skips;
};

/// One finished run: its sort key and its JSON lines (run record first).
struct RunOutput {
    std::string sort_key;
    std::string lines;
};

struct GroupResult {
    std::vector<RunOutput> runs;
    std::vector<Skip> skips;
    /// Bytes of call chunks the group wrote to its spill file, and sorted runs
    /// of calls the sort wrote; both are zero when the share held everything.
    std::uint64_t spilled_bytes = 0;
    std::uint64_t sort_runs = 0;
};

/// Walk `roots` for `nodes_<N>/ppn_<M>` run directories and describe their
/// run groups, sorted by directory then set. Problems found without reading
/// traces are reported as skips.
coro::CoroTask<Discovery> discover(CoroScope& ctx,
                                   std::vector<std::string> roots);

/// 16-digit lowercase hex FNV-1a of `app|system|unique_input|nodes|ppn|
/// papi_set`.
std::string run_id(const RunKeys& keys);

/// Read one group's traces (files in parallel) and build each complete set's
/// output, in set order. A set that fails a completeness or consistency check
/// is reported in `skips` instead.
///
/// `memory_share` bytes bound what the group holds: open readers, calls kept
/// between reading and sorting, the sort buffer, and the counter series and
/// path records. Calls beyond their quarter go to a spill file in spill_dir().
/// A group whose counter series or path records alone exceed their quarter is
/// skipped with a reason that names the share; the rest of the output is not
/// counted. A share below MIN_MEMORY_BUDGET_BYTES works, but the open readers
/// then hold more than the share.
coro::CoroTask<GroupResult> process_group(CoroScope& ctx, RunGroup group,
                                          std::uint64_t memory_share);

}  // namespace dftracer::utils::trace::genesis

#endif  // DFTRACER_UTILS_TRACE_GENESIS_GENESIS_H
