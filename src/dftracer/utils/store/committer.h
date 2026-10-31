#ifndef DFTRACER_UTILS_STORE_COMMITTER_H
#define DFTRACER_UTILS_STORE_COMMITTER_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/coro/coro.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/store/data_file.h>
#include <dftracer/utils/store/open_extent.h>
#include <dftracer/utils/store/record.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace dftracer::utils {

class Executor;

}  // namespace dftracer::utils

namespace dftracer::utils::store::engine {

class WriterExtents;

/// What one batch adds to the group commit that records it, and whether its
/// data must be on disk before its commit returns. The writer fills it,
/// ExtentWriter::append carries it as the segment context, and the committer
/// deletes it once the record naming it is written.
struct PendingBatch {
    PendingBatch();
    ~PendingBatch();
    PendingBatch(const PendingBatch&) = delete;
    PendingBatch& operator=(const PendingBatch&) = delete;

    NamespaceItem namespace_item;
    std::uint32_t namespace_id = 0;
    ScopeItem scope_item;
    std::uint32_t scope_id = 0;
    CopyItem copy_item;
    /// The count that charged this batch's segment bytes, and how many. A
    /// discarded batch gives them back there. A written extent releases them
    /// by its segments instead.
    WriterExtents* extents = nullptr;
    std::uint64_t charged_bytes = 0;
    /// The big value runs the batch's segment names. The record that lists
    /// the segment's extent lists each run as a block of the same commit, so a
    /// lost run makes that commit read as bad.
    dstd::Vector<ExtentRef> big_runs;
    bool durable = false;
    /// The CopyItem is recorded again as the state that replaces the first.
    bool publish = false;
};

/// One copy state change for Committer::on_changes. A drop hides the whole
/// scope. Any other change is a copy state with no entries.
struct QueuedChange {
    PendingBatch* batch = nullptr;
    bool drop = false;
};

/// Writes one record per group commit for the extents and copy state changes
/// the writer produces, from one task of the writer's executor. The on_*()
/// methods take their item from any thread. Records are written one at a time.
/// A record error latches, and every later commit reports it.
class Committer {
   public:
    /// A group commit starts as soon as `commit_bytes` of segment bytes, raw,
    /// are pending, and at the latest `commit_interval` after the first pending
    /// item arrived.
    Committer(DataFileWriter& file, Executor& executor,
              std::uint64_t commit_bytes,
              std::chrono::milliseconds commit_interval);
    Committer(const Committer&) = delete;
    Committer& operator=(const Committer&) = delete;
    Committer(Committer&&) = delete;
    Committer& operator=(Committer&&) = delete;
    /// Stops the task if it still runs and frees every batch whose record was
    /// never written.
    ~Committer();

    /// Starts the group commit task on the executor. Call once.
    void start();

    /// Queues a written extent. Runs on an I/O backend thread.
    void on_extent(WrittenExtent&& extent);
    /// Queues a drop of [begin, end) of a copy. An empty `end` means to the
    /// last key and both empty a whole-scope drop. Takes ownership of `batch`.
    void on_drop(PendingBatch* batch, dstd::Vector<std::uint8_t>&& begin,
                 dstd::Vector<std::uint8_t>&& end);
    /// Queues a range erase of one copy. Takes ownership of `batch`.
    void on_erase(PendingBatch* batch, EraseItem&& item);
    /// Queues copy state changes under one lock, so one record holds all of
    /// them: published building copies, failed copies and whole-scope drops.
    /// Takes ownership of every batch.
    void on_changes(dstd::Span<const QueuedChange> changes);

    /// Records everything queued now as one record and rewrites the header
    /// tail, syncing before and after it when any of those batches is durable.
    /// Returns the first record error and latches it. Cannot run concurrently
    /// with the task's own group commit.
    dstd::Expected<void, dstd::Error> commit_pending();

    /// The records written to the file so far. A reader that opened the file
    /// before a larger count lacks the entries of the later records.
    std::uint64_t records() const noexcept {
        return records_.load(std::memory_order_acquire);
    }

    /// True once a record write failed. error() is meaningful only then.
    bool failed() const noexcept {
        return failed_.load(std::memory_order_acquire);
    }
    dstd::Error error() const;

    void latch(const dstd::Error& error);

    /// Stops the task and returns once it has left, so nothing calls back into
    /// this committer afterwards.
    coro::CoroTask<dstd::Expected<void, dstd::Error>> stop();

   private:
    friend struct CommitterTestHook;

    struct PendingEntry {
        WrittenExtent extent;
        PendingBatch* batch = nullptr;
        bool has_extent = false;
        bool emit_drop = false;
        dstd::Vector<std::uint8_t> begin_key;
        dstd::Vector<std::uint8_t> end_key;
        dstd::Optional<EraseItem> erase;
    };

    struct WakeState;
    struct WaitForWork;
    struct StopWait;
    struct EntryList;

    coro::Coro run();
    void wake();
    void finish_loop();
    dstd::Expected<void, dstd::Error> sync_file();
    void drain(dstd::Vector<PendingEntry>& out);
    void free_entries(dstd::Vector<PendingEntry>& entries) noexcept;
    void add_entry(PendingEntry&& entry);
    bool work_pending() const;

    DataFileWriter& file_;
    Executor& executor_;
    std::uint64_t commit_bytes_ = 0;
    std::chrono::milliseconds commit_interval_{0};

    mutable std::mutex queue_mutex_;
    dftracer::utils::dstd::Vector<PendingEntry> entries_;
    std::uint64_t pending_bytes_ = 0;
    std::shared_ptr<WakeState> wake_state_;
    std::coroutine_handle<> stop_waiter_{};
    Executor* stop_executor_ = nullptr;
    std::condition_variable done_cv_;
    bool stopping_ = false;
    bool started_ = false;
    bool loop_done_ = false;

    std::mutex commit_mutex_;
    mutable std::mutex error_mutex_;
    std::atomic<bool> failed_{false};
    dstd::Optional<dstd::Error> latched_error_;
    std::uint64_t next_sequence_ = 1;
    std::atomic<std::uint64_t> records_{0};
    std::vector<std::uint8_t> announced_namespaces_;
    std::vector<std::uint8_t> announced_scopes_;
    std::vector<std::uint8_t> announced_copies_;
};

/// Test-only: counts the group commit syncs and fails the next record write
/// without touching the file.
struct CommitterTestHook {
    static void reset();
    static std::uint64_t syncs();
    static void fail_next_record(bool fail);
    /// PendingBatch objects alive now, which is zero once every batch was
    /// either recorded or discarded.
    static std::uint64_t live_batches();
};

}  // namespace dftracer::utils::store::engine

#endif  // DFTRACER_UTILS_STORE_COMMITTER_H
