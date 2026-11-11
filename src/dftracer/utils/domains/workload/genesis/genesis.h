// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DOMAINS_WORKLOAD_GENESIS_GENESIS_H
#define DFTRACER_UTILS_DOMAINS_WORKLOAD_GENESIS_GENESIS_H

#include <dftracer/utils/common/timer.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace dftracer::utils::domains::workload::genesis {

inline constexpr double SKETCH_ACCURACY = 0.01;

struct RunKeys {
    dstd::String app;
    dstd::String system;
    dstd::String unique_input;
    std::int64_t nodes = 0;
    std::int64_t ppn = 0;
    dstd::String papi_set;
};

/// One PAPI set of a run group. `papi_counters` (sorted) identifies the set's
/// processes when a group holds several sets; empty for single-set groups.
struct SetSpec {
    RunKeys keys;
    dstd::Vector<dstd::String> papi_counters;
    dstd::Vector<dstd::String> gpu_csvs;
};

/// Files read together: one set (matrix) or all sets of a tioga run
/// directory, whose time slices mix sets. With `sets_from_counters`, `sets`
/// holds one template whose keys lack `papi_set`, and each distinct PAPI
/// counter list found in the traces becomes a set.
struct RunGroup {
    dstd::String dir;
    dstd::String rel_dir;
    dstd::Vector<dstd::String> files;
    std::uint64_t compressed_bytes = 0;
    dstd::String summary_json;
    dstd::Vector<SetSpec> sets;
    bool sets_from_counters = false;
};

struct Skip {
    dstd::String dir;
    dstd::String file;
    dstd::String reason;
};

struct Discovery {
    dstd::Vector<RunGroup> groups;
    dstd::Vector<Skip> skips;
};

/// One finished run. Its JSON lines (run record first) are made on demand from
/// the state of its group, which stays in memory, counted in the group's share,
/// until the last RunOutput of the group is destroyed. Not thread-safe.
class RunOutput {
   public:
    class Source {
       public:
        virtual ~Source() = default;
        virtual std::size_t items() const = 0;
        /// The run and func lines of `item` go to `head`, its counter lines
        /// to `body`; both may be the same string.
        virtual void append(std::size_t item, dstd::String& head,
                            dstd::String& body) const = 0;
    };

    explicit RunOutput(dstd::UniquePtr<Source> source);
    RunOutput(RunOutput&&) noexcept;
    RunOutput& operator=(RunOutput&&) noexcept;
    ~RunOutput();

    /// Appends the next run and func lines to `head` and counter lines to
    /// `body`, whole lines of at least `bytes` in all unless the run ends
    /// first; false once no lines are left.
    bool next(dstd::String& head, dstd::String& body, std::size_t bytes);

    /// Every line of the run, from the start, each func line followed by its
    /// counter lines; does not move next().
    dstd::String lines() const;

   private:
    dstd::UniquePtr<Source> source_;
    std::size_t next_item_ = 0;
};

struct GroupResult {
    dstd::Vector<RunOutput> runs;
    dstd::Vector<Skip> skips;
    /// Bytes of call chunks the group wrote to its spill file, and sorted runs
    /// of calls the sort wrote; both are zero when the share held everything.
    std::uint64_t spilled_bytes = 0;
    std::uint64_t sort_runs = 0;
    /// A skip came from the share: the same group may succeed with a larger
    /// one.
    bool over_budget = false;
};

/// Live counters of one group, for a progress display. process_group updates
/// them from worker threads, once per file read or chunk of calls, and calls
/// `on_update` after each update, so `on_update` must be thread-safe and
/// quick. The caller resets them before each group.
struct GroupProgress {
    /// WAIT: the caller waits for its memory share. READ: its files are read.
    /// SORT: its calls go into the sort. WALK: the sorted calls are walked.
    /// WRITE: the caller writes its runs.
    enum class Phase : std::uint8_t { Wait, Read, Sort, Walk, Write };

    std::atomic<Phase> phase{Phase::Wait};
    /// steady_clock time, in nanoseconds, when `phase` was entered.
    std::atomic<std::int64_t> phase_since_ns{0};
    /// Compressed bytes of the files read so far.
    std::atomic<std::uint64_t> read_bytes{0};
    /// Calls of the group, known once its files are read.
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> sorted{0};
    std::atomic<std::uint64_t> walked{0};
    dstd::Function<void()> on_update;

    void enter(Phase p) noexcept {
        phase_since_ns.store(common::steady_now_ns(),
                             std::memory_order_relaxed);
        phase.store(p, std::memory_order_relaxed);
    }

    void reset() noexcept {
        read_bytes.store(0, std::memory_order_relaxed);
        calls.store(0, std::memory_order_relaxed);
        sorted.store(0, std::memory_order_relaxed);
        walked.store(0, std::memory_order_relaxed);
        enter(Phase::Wait);
    }

    void updated() const {
        if (on_update) on_update();
    }
};

/// Walk `roots` for `nodes_<N>/ppn_<M>` run directories and describe their
/// run groups, sorted by directory then set. Problems found without reading
/// traces are reported as skips. A root or run directory that cannot be
/// resolved, or a failed walk, gives the error of that call.
coro::CoroTask<dstd::Result<Discovery>> discover(
    coro::Scope& ctx, dstd::Vector<dstd::String> roots);

/// 16-digit lowercase hex XXH3-64 of `app|system|unique_input|nodes|ppn|
/// papi_set`.
dstd::String run_id(const RunKeys& keys);

/// Read one group's traces (files in parallel) and build each complete set's
/// output, in set order. A set that fails a completeness or consistency check
/// is reported in `skips` instead.
///
/// `memory_share` bytes bound what the group holds: open readers, calls kept
/// between reading and sorting, the sort buffer, and the counter series and
/// path records. Calls beyond their quarter go to a spill file in spill_dir().
/// A set whose counter series or path records alone exceed their quarter is
/// skipped with a reason that names the share, and `over_budget` is set; the
/// rest of the output is not counted. A share below
/// memory::MIN_MEMORY_BUDGET_BYTES works, but the open readers then hold more
/// than the share.
///
/// `progress`, when given, is updated as the group goes and must outlive the
/// call.
/// Asked for `bytes` beyond the share when the group's counter series, path
/// records or counter statistics run short; returns the bytes it grants, 0
/// when it has none to spare. It must not wait and must be thread-safe; the
/// caller keeps track of the bytes it granted and gets them back once the
/// group's runs are done.
using BorrowFn = dstd::Function<std::uint64_t(std::uint64_t bytes)>;

/// `borrow`, when given, is asked before a set is failed for its share, so a
/// group whose state outgrows the share takes free memory of the budget
/// instead of being run again with a larger share.
coro::CoroTask<GroupResult> process_group(coro::Scope& ctx, RunGroup group,
                                          std::uint64_t memory_share,
                                          GroupProgress* progress = nullptr,
                                          const BorrowFn* borrow = nullptr);

}  // namespace dftracer::utils::domains::workload::genesis

#endif  // DFTRACER_UTILS_DOMAINS_WORKLOAD_GENESIS_GENESIS_H
