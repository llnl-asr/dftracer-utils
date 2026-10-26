#ifndef DFTRACER_UTILS_INDEX_STORE_ENGINE_COMMITTER_H
#define DFTRACER_UTILS_INDEX_STORE_ENGINE_COMMITTER_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/store/engine/data_file.h>
#include <dftracer/utils/index/store/engine/open_page.h>
#include <dftracer/utils/index/store/engine/record.h>
#include <dftracer/utils/runtime/coro/coro.h>
#include <dftracer/utils/runtime/coro/task.h>

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

namespace dftracer::utils::index::store::engine {

class WriterPages;

/// What one batch adds to the group commit that records it: the namespace,
/// scope and copy items it is the first to name, and whether its data must be
/// on disk before its commit returns. The writer fills it, PageWriter::append
/// carries it as the section context, and the committer deletes it once the
/// record naming it is written.
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
    /// The count that charged this batch's section bytes, and how many. A
    /// discarded batch gives them back there; a written page releases them
    /// by its sections instead.
    WriterPages* pages = nullptr;
    std::uint64_t charged_bytes = 0;
    /// The big value runs the batch's section names. The record that lists
    /// the section's page lists each run as a block of the same commit, so a
    /// lost run makes that commit read as bad.
    dstd::Vector<PageRef> big_runs;
    bool durable = false;
};

/// Writes one record per group commit for the pages and copy state changes the
/// writer produces, from one task of the writer's executor.
///
/// on_page(), on_drop() and on_fail() take their item from any thread, and
/// commit_pending() is called by the writer's flush and close; only the task
/// and those callers write records, one at a time. A record error latches, and
/// every later commit reports it.
class Committer {
   public:
    /// A group commit starts as soon as `commit_bytes` of page bytes are
    /// pending, and at the latest `commit_interval` after the first pending
    /// item arrived.
    Committer(DataFileWriter& file, Executor& executor,
              std::uint64_t commit_bytes,
              std::chrono::milliseconds commit_interval);
    Committer(const Committer&) = delete;
    Committer& operator=(const Committer&) = delete;
    Committer(Committer&&) = delete;
    Committer& operator=(Committer&&) = delete;
    /// Stops the task if it still runs, and frees every batch whose record was
    /// never written.
    ~Committer();

    /// Starts the group commit task on the executor. Call once.
    void start();

    /// Queues a written page. Runs on an I/O backend thread.
    void on_page(WrittenPage&& page);
    /// Queues a drop of [begin, end) of a copy; an empty `end` means to the
    /// last key and both empty a whole-scope drop. Takes ownership of `batch`.
    void on_drop(PendingBatch* batch, dstd::Vector<std::uint8_t>&& begin,
                 dstd::Vector<std::uint8_t>&& end);
    /// Queues a copy in state Failed with no entries. Takes ownership of
    /// `batch`.
    void on_fail(PendingBatch* batch);

    /// Records everything queued now as one record and rewrites the header
    /// tail, syncing before and after it when any of those batches is durable.
    /// Returns the first record error and latches it. Cannot run concurrently
    /// with the task's own group commit.
    dstd::Expected<void, dstd::Error> commit_pending();

    /// True once a record write failed, so every later commit must fail too.
    bool failed() const noexcept {
        return failed_.load(std::memory_order_acquire);
    }
    /// The first record error. Only meaningful once failed() is true.
    dstd::Error error() const;

    /// Latches `error` as a record error without writing one.
    void latch(const dstd::Error& error);

    /// Stops the task and returns once it has left, so nothing calls back into
    /// this committer afterwards.
    coro::CoroTask<dstd::Expected<void, dstd::Error>> stop();

   private:
    friend struct CommitterTestHook;

    /// One queued item: a written page, or a copy that was dropped or failed.
    struct PendingEntry {
        WrittenPage page;
        PendingBatch* batch = nullptr;
        bool has_page = false;
        bool emit_drop = false;
        dstd::Vector<std::uint8_t> begin_key;
        dstd::Vector<std::uint8_t> end_key;
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

    /// Guards the queue, the wake state and the loop's lifecycle.
    mutable std::mutex queue_mutex_;
    dftracer::utils::dstd::Vector<PendingEntry> entries_;
    std::uint64_t pending_bytes_ = 0;
    std::shared_ptr<WakeState> wake_state_;
    std::coroutine_handle<> stop_waiter_{};
    std::condition_variable done_cv_;
    bool stopping_ = false;
    bool started_ = false;
    bool loop_done_ = false;

    std::mutex commit_mutex_;
    mutable std::mutex error_mutex_;
    std::atomic<bool> failed_{false};
    dstd::Optional<dstd::Error> latched_error_;
    std::uint64_t next_sequence_ = 1;
    std::vector<std::uint8_t> announced_namespaces_;
    std::vector<std::uint8_t> announced_scopes_;
    std::vector<std::uint8_t> announced_copies_;
};

/// Test-only: counts the group commit syncs and fails the next record write
/// without touching the file.
struct CommitterTestHook {
    static void reset();
    static std::uint64_t syncs();
    /// Makes the next group commit fail at its record write.
    static void fail_next_record(bool fail);
    /// PendingBatch objects alive now, which is zero once every batch was
    /// either recorded or discarded.
    static std::uint64_t live_batches();
};

}  // namespace dftracer::utils::index::store::engine

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_COMMITTER_H
