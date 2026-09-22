#ifndef DFTRACER_UTILS_TRACE_VIEWS_FOLD_H
#define DFTRACER_UTILS_TRACE_VIEWS_FOLD_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/trace/views/coverage.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view_scan.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// A plugin fold stashes the dftu_task* its on_batch returned for the async
// driver to await; forward-declared to keep the C ABI out of this header.
struct dftu_task;

// The fold-fusion scan core; see the project_fold_fuse_architecture memory.
namespace dftracer::utils::trace::views::detail {

struct FoldBatch {
    std::span<const FoldEvent> events;  // valid for the `step` call only
    const ScanUnit& unit;
    // Raw NDJSON lines, populated only when a wants_raw() fold is present, in
    // scan order. A raw fold parses these itself (arbitrary-JSON access); a
    // FoldEvent fold ignores them. Valid for the `step` call only.
    std::span<const std::string_view> raw = {};
};

/// Per-worker keyed byte scratchpad so a producer fold can hand a derived value
/// to a later consumer fold in the same batch. Single-threaded, unlocked;
/// consume borrows the bytes until the next clear.
class FoldPortBus {
   public:
    void publish(std::uint64_t key, const void* data, std::uint32_t len);
    /// NULL if `key` was not published since the last clear.
    const void* consume(std::uint64_t key, std::uint32_t* out_len) const;
    void clear();

    /// The batch as a frame after the last transforming fold, for every fold
    /// stepped after it in the same batch; unset until a fold rewrites it,
    /// and each later fold then reads this instead of the events.
    std::optional<dftracer::utils::dataframe::DataFrame> frame;

   private:
    struct Entry {
        std::size_t offset;
        std::uint32_t len;
    };
    std::unordered_map<std::uint64_t, Entry> index_;
    std::vector<std::byte> arena_;
};

/// A push Sink over the fused scan, batch-grained so the virtual call amortizes
/// across the batch. A returning fold exposes a typed result the caller pulls
/// after fuse; a persisting fold writes inside finalize.
class Fold {
   public:
    virtual ~Fold() = default;

    /// The scan must already deliver what this fold needs; it is never widened
    /// to satisfy one.
    virtual bool accepts(const ScanShape& shape) const = 0;

    /// ORed across folds, so args are extracted only when some fold needs them.
    virtual bool needs_args() const { return false; }

    /// A fold that consumes FoldBatch::raw (raw NDJSON lines it parses itself)
    /// rather than the parsed FoldEvent stream. ORed across folds so the scan
    /// keeps raw lines when any is present.
    virtual bool wants_raw() const { return false; }

    /// A fold that reads FoldEvent::schema_leaves (every scalar leaf of the
    /// record, arbitrarily nested) for schemaless column discovery. ORed across
    /// folds; the scan enumerates leaves only when some fold asks, so the query
    /// path pays nothing.
    virtual bool wants_schema() const { return false; }

    /// Extra fields (top-level type/ph/id, or nested a.b/a[0]) this fold needs
    /// captured into each event beyond what the POD carries and beyond flat
    /// args (which needs_args() already covers). Merged with the plan's own
    /// captures.
    virtual std::vector<std::string> extra_captures() const { return {}; }

    /// One per worker slot, folded lock-free, merged after the fan-out joins.
    virtual std::unique_ptr<Fold> slice() const = 0;

    virtual void step(const FoldBatch& batch) = 0;
    virtual void seal_unit(const ScanUnit& unit) = 0;
    virtual void drop_unit(const ScanUnit& unit) = 0;
    virtual void merge(Fold& slice) = 0;

    /// `covered` bounds what a fold may claim. Returns false to persist
    /// nothing.
    virtual coro::CoroTask<bool> finalize(const CoverageSet& covered) = 0;

    /// The async task the last `step` produced, or null when synchronous; the
    /// driver awaits it. Only a plugin fold returns non-null.
    virtual ::dftu_task* take_pending() { return nullptr; }

    /// The fuse driver binds its per-worker bus so a fold may publish/consume
    /// per-batch values; folds that do not communicate ignore it.
    virtual void bind_port_bus(FoldPortBus*) {}
};

/// Advisory, mutable narrowing state a running fuse() scan consults before
/// claiming each remaining unit: once populated, fuse_worker skips an
/// excluded file or checkpoint without decompressing it. This is the backing
/// state behind dataframe::Cursor::narrow() - a source (ViewSource's
/// streaming cursor) writes exclusions into it, computed with the same
/// bloom/dictionary chunk-pruning path gather_units uses for the static
/// prune, while the scan gather_units already started is still draining
/// `units`. Thread-safe: writers (the cursor's narrow() calls) and readers
/// (every fuse worker) run concurrently.
class DynamicPrune {
   public:
    /// Excludes every checkpoint of `file_path` from the remaining scan.
    void exclude_file(const std::string& file_path);
    /// Excludes `checkpoints` of `file_path`; merges into any prior
    /// exclusion for the same file.
    void exclude_checkpoints(const std::string& file_path,
                             std::vector<std::uint64_t> checkpoints);
    bool is_excluded(const std::string& file_path,
                     std::uint64_t checkpoint_idx) const;
    /// Called by a fuse worker for each unit it skips because of this prune.
    void record_skip() { skipped_.fetch_add(1, std::memory_order_relaxed); }
    std::uint64_t skipped() const {
        return skipped_.load(std::memory_order_relaxed);
    }

   private:
    mutable std::mutex mu_;
    std::unordered_set<std::string> excluded_files_;
    std::unordered_map<std::string, std::unordered_set<std::uint64_t>>
        excluded_checkpoints_;
    std::atomic<std::uint64_t> skipped_{0};
};

/// One traversal driving every fold with per-worker slices merged at the end.
/// `intern` is shared across workers and caller-owned; every fold must resolve
/// ids through it. Units `covered` already accounts for are skipped.
/// `dyn_prune` (optional), when non-null, is polled per unit so a cursor's
/// narrow() call mid-scan can still take effect on units not yet claimed.
coro::CoroTask<ExportStats> fuse(const ViewPlan& plan,
                                 const ViewDefinition& vdef,
                                 std::span<Fold* const> folds,
                                 dftracer::utils::StringIntern& intern,
                                 const CoverageSet* covered = nullptr,
                                 std::uint64_t limit = 0,
                                 DynamicPrune* dyn_prune = nullptr);

/// Fields the scanner must capture into each event's args for this plan: the
/// non-scalar Field group keys and nested agg fields the POD does not natively
/// carry (type/ph, a.b, a[0]). Empty for a plain plan.
std::vector<std::string> extra_capture_fields(const ViewPlan& plan);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_FOLD_H
