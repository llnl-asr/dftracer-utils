#ifndef DFTRACER_UTILS_TRACE_GENESIS_GENESIS_H
#define DFTRACER_UTILS_TRACE_GENESIS_GENESIS_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
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
        virtual void append(std::size_t item, std::string& head,
                            std::string& body) const = 0;
    };

    explicit RunOutput(std::unique_ptr<Source> source);
    RunOutput(RunOutput&&) noexcept;
    RunOutput& operator=(RunOutput&&) noexcept;
    ~RunOutput();

    /// Appends the next run and func lines to `head` and counter lines to
    /// `body`, whole lines of at least `bytes` in all unless the run ends
    /// first; false once no lines are left.
    bool next(std::string& head, std::string& body, std::size_t bytes);

    /// Every line of the run, from the start, each func line followed by its
    /// counter lines; does not move next().
    std::string lines() const;

   private:
    std::unique_ptr<Source> source_;
    std::size_t next_item_ = 0;
};

struct GroupResult {
    std::vector<RunOutput> runs;
    std::vector<Skip> skips;
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
    enum class Phase : std::uint8_t { WAIT, READ, SORT, WALK, WRITE };

    std::atomic<Phase> phase{Phase::WAIT};
    /// steady_clock time, in nanoseconds, when `phase` was entered.
    std::atomic<std::int64_t> phase_since_ns{0};
    /// Compressed bytes of the files read so far.
    std::atomic<std::uint64_t> read_bytes{0};
    /// Calls of the group, known once its files are read.
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> sorted{0};
    std::atomic<std::uint64_t> walked{0};
    std::function<void()> on_update;

    void enter(Phase p) noexcept {
        phase_since_ns.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_relaxed);
        phase.store(p, std::memory_order_relaxed);
    }

    void reset() noexcept {
        read_bytes.store(0, std::memory_order_relaxed);
        calls.store(0, std::memory_order_relaxed);
        sorted.store(0, std::memory_order_relaxed);
        walked.store(0, std::memory_order_relaxed);
        enter(Phase::WAIT);
    }

    void updated() const {
        if (on_update) on_update();
    }
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
/// A set whose counter series or path records alone exceed their quarter is
/// skipped with a reason that names the share, and `over_budget` is set; the
/// rest of the output is not counted. A share below MIN_MEMORY_BUDGET_BYTES
/// works, but the open readers then hold more than the share.
///
/// `progress`, when given, is updated as the group goes and must outlive the
/// call.
/// Asked for `bytes` beyond the share when the group's counter series, path
/// records or counter statistics run short; returns the bytes it grants, 0
/// when it has none to spare. It must not wait and must be thread-safe; the
/// caller keeps track of the bytes it granted and gets them back once the
/// group's runs are done.
using BorrowFn = std::function<std::uint64_t(std::uint64_t bytes)>;

/// `borrow`, when given, is asked before a set is failed for its share, so a
/// group whose state outgrows the share takes free memory of the budget
/// instead of being run again with a larger share.
coro::CoroTask<GroupResult> process_group(CoroScope& ctx, RunGroup group,
                                          std::uint64_t memory_share,
                                          GroupProgress* progress = nullptr,
                                          const BorrowFn* borrow = nullptr);

}  // namespace dftracer::utils::trace::genesis

#endif  // DFTRACER_UTILS_TRACE_GENESIS_GENESIS_H
