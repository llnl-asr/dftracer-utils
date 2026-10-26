#include <dftracer/utils/index/store/engine/committer.h>
#include <dftracer/utils/index/store/engine/io_fault.h>
#include <dftracer/utils/runtime/coro/completion_latch.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <utility>

namespace dftracer::utils::index::store::engine {

namespace {

std::atomic<std::uint64_t>& sync_calls() {
    static std::atomic<std::uint64_t> calls{0};
    return calls;
}

std::atomic<bool>& record_failure_armed() {
    static std::atomic<bool> armed{false};
    return armed;
}

/// Live PendingBatch objects, so a test can prove that a page write failure
/// and a teardown leave none behind.
std::atomic<std::uint64_t>& batch_count() {
    static std::atomic<std::uint64_t> count{0};
    return count;
}

dstd::Error io_error(const char* message) {
    return dstd::Error(dstd::Condition::Io, message);
}

/// True when `id` had not been announced yet. Ids are dense from 1, so one
/// grown byte vector answers this without hashing.
bool announce(std::vector<std::uint8_t>& announced, std::uint32_t id) {
    if (id >= announced.size()) {
        announced.resize(static_cast<std::size_t>(id) + 1U, 0);
    }
    if (announced[id] != 0) return false;
    announced[id] = 1;
    return true;
}

}  // namespace

PendingBatch::PendingBatch() {
    batch_count().fetch_add(1, std::memory_order_relaxed);
}

PendingBatch::~PendingBatch() {
    batch_count().fetch_sub(1, std::memory_order_relaxed);
}

/// One suspended committer task: the latch makes exactly one of the wake and
/// the timer resume it.
struct Committer::WakeState {
    coro::CompletionLatch latch;
    /// One wait has two completion sources, the write that reaches
    /// `commit_bytes` and the interval. Only the first may resume, or the
    /// coroutine would run twice on one frame.
    std::atomic<bool> claimed{false};
    std::coroutine_handle<> handle;
    Executor* executor = nullptr;
    TimerService* timers = nullptr;
    std::atomic<TimerService::TimerId> timer{0};

    void cancel_timer() {
        const TimerService::TimerId id =
            timer.exchange(0, std::memory_order_acq_rel);
        if (id != 0 && timers != nullptr) timers->cancel_timeout(id);
    }

    void on_wake() { finish(); }
    void on_fire() { finish(); }

    void finish() {
        if (claimed.exchange(true, std::memory_order_acq_rel)) return;
        if (latch.on_completed()) coro::resume_continuation(executor, handle);
    }
};

/// Waits until `commit_bytes` of page bytes are pending or `commit_interval`
/// passed. The threshold is read under the queue lock that the producer adds
/// the counter under, so a wake that arrives first is not lost.
struct Committer::WaitForWork {
    Committer& committer;
    std::shared_ptr<WakeState> state = std::make_shared<WakeState>();

    bool await_ready() const {
        std::lock_guard<std::mutex> lock(committer.queue_mutex_);
        return committer.stopping_ ||
               committer.pending_bytes_ >= committer.commit_bytes_;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        Committer& self = committer;
        state->handle = handle;
        state->executor = &self.executor_;
        TimerService& timers = self.executor_.get_timer_service();
        state->timers = &timers;
        state->timer.store(
            timers.register_timeout(self.commit_interval_,
                                    [state = state] { state->on_fire(); }),
            std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(self.queue_mutex_);
            if (self.stopping_ || self.pending_bytes_ >= self.commit_bytes_) {
                return false;
            }
            self.wake_state_ = state;
        }
        const std::uint8_t before = state->latch.on_suspended();
        return (before & (coro::CompletionLatch::BIT_COMPLETED |
                          coro::CompletionLatch::BIT_CANCELLED)) == 0;
    }

    void await_resume() {
        state->cancel_timer();
        std::lock_guard<std::mutex> lock(committer.queue_mutex_);
        if (committer.wake_state_ == state) committer.wake_state_.reset();
    }
};

/// Waits for the task to leave `run()`.
struct Committer::StopWait {
    Committer& committer;

    bool await_ready() const {
        std::lock_guard<std::mutex> lock(committer.queue_mutex_);
        return committer.loop_done_;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        std::lock_guard<std::mutex> lock(committer.queue_mutex_);
        if (committer.loop_done_) return false;
        committer.stop_waiter_ = handle;
        return true;
    }

    void await_resume() {}
};

Committer::Committer(DataFileWriter& file, Executor& executor,
                     std::uint64_t commit_bytes,
                     std::chrono::milliseconds commit_interval)
    : file_(file),
      executor_(executor),
      commit_bytes_(commit_bytes),
      commit_interval_(commit_interval) {}

Committer::~Committer() {
    bool wait = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (started_ && !loop_done_) {
            stopping_ = true;
            wait = true;
        } else {
            loop_done_ = true;
        }
    }
    if (wait) {
        wake();
        BlockingRegion block;
        std::unique_lock<std::mutex> lock(queue_mutex_);
        done_cv_.wait(lock, [this] { return loop_done_; });
    }
    dstd::Vector<PendingEntry> leftover;
    drain(leftover);
    free_entries(leftover);
}

void Committer::start() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (started_) return;
        started_ = true;
    }
    coro::Coro loop = run();
    auto handle = loop.release();
    handle.promise().executor = &executor_;
    handle.resume();
}

coro::Coro Committer::run() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (stopping_) break;
        }
        // A named awaitable: GCC 12.1 destroys a co_await temporary that has
        // a non-trivial destructor twice.
        WaitForWork wait{*this};
        co_await wait;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (stopping_) break;
        }
        commit_pending();
    }
    finish_loop();
}

void Committer::finish_loop() {
    std::coroutine_handle<> waiter;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        loop_done_ = true;
        wake_state_.reset();
        waiter = stop_waiter_;
        stop_waiter_ = nullptr;
        // Notified under the lock: the destructor returns as soon as it sees
        // loop_done_, so it must not read the flag before the notify that
        // wakes its wait on the condition variable is under way.
        done_cv_.notify_all();
    }
    if (waiter) coro::resume_continuation(&executor_, waiter);
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Committer::stop() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stopping_ = true;
        if (!started_) loop_done_ = true;
    }
    wake();
    StopWait stop{*this};
    co_await stop;
    if (failed()) co_return dstd::Unexpected<dstd::Error>(error());
    co_return {};
}

void Committer::wake() {
    std::shared_ptr<WakeState> state;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        state = wake_state_;
    }
    if (state) state->on_wake();
}

void Committer::add_entry(PendingEntry&& entry) {
    const std::uint64_t bytes = entry.has_page ? entry.page.length : 0;
    bool wake_now = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        entries_.push_back(std::move(entry));
        pending_bytes_ += bytes;
        wake_now = pending_bytes_ >= commit_bytes_;
    }
    if (wake_now) wake();
}

void Committer::on_page(WrittenPage&& page) {
    PendingEntry entry;
    entry.page = std::move(page);
    entry.has_page = true;
    add_entry(std::move(entry));
}

void Committer::on_drop(PendingBatch* batch, dstd::Vector<std::uint8_t>&& begin,
                        dstd::Vector<std::uint8_t>&& end) {
    PendingEntry entry;
    entry.batch = batch;
    entry.emit_drop = true;
    entry.begin_key = std::move(begin);
    entry.end_key = std::move(end);
    add_entry(std::move(entry));
}

void Committer::on_fail(PendingBatch* batch) {
    PendingEntry entry;
    entry.batch = batch;
    add_entry(std::move(entry));
}

void Committer::drain(dstd::Vector<PendingEntry>& out) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    out.swap(entries_);
    entries_.clear();
    pending_bytes_ = 0;
}

void Committer::free_entries(dstd::Vector<PendingEntry>& entries) noexcept {
    for (PendingEntry& entry : entries) {
        if (entry.has_page) {
            for (void* context : entry.page.contexts) {
                delete static_cast<PendingBatch*>(context);
            }
            entry.page.contexts.clear();
        }
        delete entry.batch;
        entry.batch = nullptr;
    }
}

dstd::Expected<void, dstd::Error> Committer::sync_file() {
    const int fd = file_.fd();
    if (fd < 0) {
        return dstd::Unexpected<dstd::Error>(
            io_error("store group commit sync on a closed file"));
    }
    sync_calls().fetch_add(1, std::memory_order_relaxed);
    int result = -1;
    do {
#if defined(__APPLE__)
        result = ::fsync(fd);
#else
        result = ::fdatasync(fd);
#endif
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        return dstd::Unexpected<dstd::Error>(
            io_error("store group commit sync failed"));
    }
    io_fault::on_sync(fd);
    return {};
}

void Committer::latch(const dstd::Error& error) {
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        if (!latched_error_) latched_error_ = error;
    }
    failed_.store(true, std::memory_order_release);
}

dstd::Error Committer::error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    if (latched_error_) return *latched_error_;
    return io_error("store group commit failed");
}

dstd::Expected<void, dstd::Error> Committer::commit_pending() {
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    dstd::Vector<PendingEntry> drained;
    struct FreeOnExit {
        Committer& owner;
        dstd::Vector<PendingEntry>& entries;
        ~FreeOnExit() { owner.free_entries(entries); }
    } free_on_exit{*this, drained};
    drain(drained);

    if (drained.empty()) {
        if (failed()) return dstd::Unexpected<dstd::Error>(error());
        return {};
    }
    if (failed()) return dstd::Unexpected<dstd::Error>(error());

    bool durable = false;
    for (const PendingEntry& entry : drained) {
        if (entry.has_page) {
            for (void* context : entry.page.contexts) {
                const auto* batch = static_cast<const PendingBatch*>(context);
                if (batch != nullptr && batch->durable) durable = true;
            }
        } else if (entry.batch != nullptr && entry.batch->durable) {
            durable = true;
        }
    }

    if (durable) {
        auto synced = sync_file();
        if (!synced) {
            latch(synced.error());
            return synced;
        }
    }

    const std::uint64_t sequence = next_sequence_;
    for (PendingEntry& entry : drained) {
        if (entry.has_page) {
            for (void* context : entry.page.contexts) {
                auto* batch = static_cast<PendingBatch*>(context);
                if (batch == nullptr) continue;
                if (announce(announced_namespaces_, batch->namespace_id)) {
                    file_.add(std::move(batch->namespace_item));
                }
                if (announce(announced_scopes_, batch->scope_id)) {
                    file_.add(std::move(batch->scope_item));
                }
                if (batch->copy_item.local_id != 0 &&
                    announce(announced_copies_, batch->copy_item.local_id)) {
                    file_.add(std::move(batch->copy_item));
                }
            }
            for (void* context : entry.page.contexts) {
                auto* batch = static_cast<PendingBatch*>(context);
                if (batch == nullptr) continue;
                for (const PageRef& run : batch->big_runs) {
                    PageRefItem big;
                    big.offset = run.offset;
                    big.length = run.length;
                    big.crc = run.crc;
                    big.sequence = sequence;
                    file_.add(std::move(big));
                }
            }
            PageRefItem reference;
            reference.offset = entry.page.offset;
            reference.length = entry.page.length;
            reference.crc = entry.page.crc;
            reference.sequence = sequence;
            reference.sections = std::move(entry.page.sections);
            file_.add(std::move(reference));
            continue;
        }

        PendingBatch& batch = *entry.batch;
        if (announce(announced_namespaces_, batch.namespace_id)) {
            file_.add(std::move(batch.namespace_item));
        }
        if (announce(announced_scopes_, batch.scope_id)) {
            file_.add(std::move(batch.scope_item));
        }
        if (batch.copy_item.local_id != 0 &&
            announce(announced_copies_, batch.copy_item.local_id)) {
            file_.add(std::move(batch.copy_item));
        }
        if (entry.emit_drop) {
            DropItem item;
            item.namespace_id = batch.namespace_id;
            item.scope_id = batch.scope_id;
            item.generation = batch.copy_item.generation;
            item.begin_key = std::move(entry.begin_key);
            item.end_key = std::move(entry.end_key);
            file_.add(std::move(item));
        }
    }

    dstd::Expected<void, dstd::Error> recorded;
    if (record_failure_armed().exchange(false, std::memory_order_acq_rel)) {
        recorded = dstd::Unexpected<dstd::Error>(
            io_error("store group commit record write failed"));
    } else {
        recorded = file_.commit();
    }
    if (!recorded) {
        latch(recorded.error());
        return recorded;
    }
    if (durable) {
        auto synced = sync_file();
        if (!synced) {
            latch(synced.error());
            return synced;
        }
    }
    ++next_sequence_;
    return {};
}

void CommitterTestHook::reset() {
    sync_calls().store(0, std::memory_order_relaxed);
    record_failure_armed().store(false, std::memory_order_relaxed);
}

std::uint64_t CommitterTestHook::syncs() {
    return sync_calls().load(std::memory_order_relaxed);
}

void CommitterTestHook::fail_next_record(bool fail) {
    record_failure_armed().store(fail, std::memory_order_relaxed);
}

std::uint64_t CommitterTestHook::live_batches() {
    return batch_count().load(std::memory_order_relaxed);
}

}  // namespace dftracer::utils::index::store::engine
