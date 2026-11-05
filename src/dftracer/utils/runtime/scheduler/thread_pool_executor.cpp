// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/common/timer.h>
#include <dftracer/utils/coro/detail/monitor.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/memory/detail/heap.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/runtime/io/io_backend_factory.h>
#include <dftracer/utils/runtime/scheduler/thread_pool_executor.h>

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace dftracer::utils {

ThreadPoolExecutor::ThreadPoolExecutor(const ExecutorConfig& config)
    : last_progress_ns_(common::steady_now_ns()),
      idle_timeout_(config.idle_timeout),
      deadlock_timeout_s_(config.deadlock_timeout.count()),
      keepalive_(config.elastic_keepalive),
      task_history_(config.task_history),
      io_backend_type_(config.io_backend_type),
      io_batch_threshold_(config.io_batch_threshold) {
    std::size_t threads = config.thread_count == 0
                              ? common::hardware_concurrency()
                              : config.thread_count;
    if (threads == 0) {
        threads = 2;
    }
    std::size_t io_threads =
        config.io_pool_size == 0 ? threads : config.io_pool_size;
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    // Per-Executor thread churn dominates runtime when Valgrind
    // serializes and instruments every thread, so cap the pools.
    if (threads > 2) threads = 2;
    if (io_threads > 2) io_threads = 2;
#endif
    io_pool_size_.store(io_threads, std::memory_order_relaxed);
    thread_count_.store(threads, std::memory_order_relaxed);
    const bool eager = config.min_workers == 0 || config.min_workers > threads;
    eager_.store(eager, std::memory_order_relaxed);
    min_workers_.store(eager ? threads : config.min_workers,
                       std::memory_order_relaxed);
    max_live_.store(threads + MAX_BLOCKING_REPLACEMENTS,
                    std::memory_order_relaxed);
    DFTRACER_UTILS_LOG_DEBUG(
        "Executor created with %zu threads, idle_timeout=%lld s, "
        "deadlock_timeout=%lld s",
        threads, static_cast<long long>(idle_timeout_.count()),
        static_cast<long long>(config.deadlock_timeout.count()));
}

ThreadPoolExecutor::~ThreadPoolExecutor() {
    shutdown();
    while (foreign_enqueues_.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
}

void ThreadPoolExecutor::start() {
    if (running_) {
        DFTRACER_UTILS_LOG_WARN("%s", "Executor already running");
        return;
    }

    std::lock_guard<std::mutex> resize(resize_mutex_);
    running_ = true;
    shutdown_token_->store(false, std::memory_order_release);
    const std::size_t threads = thread_count_.load(std::memory_order_acquire);
    const std::size_t floor = min_workers_.load(std::memory_order_acquire);
    workers_.clear();
    workers_.reserve(threads);
    permits_.store(static_cast<std::ptrdiff_t>(threads),
                   std::memory_order_release);

    timer_service_.start();

    // Create and start I/O backend before workers so workers
    // can use it immediately.
    io_backend_ = runtime::io::make_io_backend(
        *this, io_pool_size_.load(), io_backend_type_, io_batch_threshold_,
        {eager_.load(), keepalive_});
    io_backend_->start();

    // Spawn the floor (== the cap in eager mode). The elastic monitor grows the
    // rest on demand. Contexts first so workers_ is stable before any worker
    // thread can iterate it.
    for (std::size_t i = 0; i < floor; ++i) {
        auto worker = dstd::make_unique<WorkerContext>(
            next_worker_id_.fetch_add(1, std::memory_order_relaxed));
        worker->last_activity = std::chrono::steady_clock::now();
        worker->owner = this;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            worker->core = acquire_core();
        }
        workers_.push_back(std::move(worker));
    }
    live_workers_.store(workers_.size(), std::memory_order_release);

    // Start worker threads after all contexts are in place.
    for (auto& worker : workers_) {
        worker->thread =
            dstd::Thread([this, ctx = worker.get()] { worker_thread(ctx); });
    }

    // Elastic mode only: a monitor drives grow/shrink between the floor and
    // cap.
    if (floor < threads) {
        monitor_thread_ = dstd::Thread([this] { monitor_loop(); });
    }

    // Reclaims permits from long blocks; blocking can occur in any mode.
    sysmon_thread_ = dstd::Thread([this] { sysmon_loop(); });

    DFTRACER_UTILS_LOG_DEBUG(
        "Executor started with %zu worker threads (cap %zu)", floor, threads);
}

void ThreadPoolExecutor::shutdown() {
    // Everything spawned from here on must not run, an executor that never
    // started included: it has no thread to run it.
    shutdown_token_->store(true, std::memory_order_release);
    if (!running_) {
        return;
    }

    DFTRACER_UTILS_LOG_DEBUG("%s", "Shutting down executor");
    cancel_in_flight();
    quiesce();
    running_ = false;
    wake_all_workers();

    coord_cv_.notify_all();
    {
        // set_threads may start the monitor. It checks running_ under this.
        std::lock_guard<std::mutex> resize(resize_mutex_);
        if (monitor_thread_.joinable()) monitor_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(sysmon_mutex_);
        sysmon_cv_.notify_all();
    }
    if (sysmon_thread_.joinable()) sysmon_thread_.join();

    // Not held across resize_mutex_ below: set_threads takes resize_mutex_
    // and then workers_mutex_. spawn_worker adds nothing once running_ is
    // false, so the list cannot grow between the two locks.
    {
        std::lock_guard<std::mutex> workers_lock(workers_mutex_);
        for (auto& worker : workers_) {
            if (worker->thread.joinable()) {
                worker->thread.join();
            }
        }
    }

    {
        // grow_io_threads reads io_backend_ under this.
        std::lock_guard<std::mutex> resize(resize_mutex_);
        if (io_backend_) {
            io_backend_->stop();
            io_backend_.reset();
        }
    }

    // Every worker moved its queues here before it exited.
    {
        void* orphans[INJECT_BATCH_MAX];
        while (const std::size_t count =
                   inject_.pop_batch(orphans, INJECT_BATCH_MAX)) {
            for (std::size_t i = 0; i < count; ++i) {
                std::coroutine_handle<>::from_address(orphans[i]).destroy();
            }
        }
    }

    {
        std::lock_guard<std::mutex> workers_lock(workers_mutex_);
        workers_.clear();
        core_count_.store(0, std::memory_order_release);
        core_list_.store(nullptr, std::memory_order_release);
        free_cores_.clear();
        cores_.clear();
        core_lists_.clear();
    }
    live_workers_.store(0, std::memory_order_release);
    timer_service_.stop();

    // Drain the main thread's thread-local destroy list (for
    // coroutines whose FinalAwaiter ran on the main thread).
    coro::drain_thread_local_destroys();

    DFTRACER_UTILS_LOG_DEBUG("%s", "Executor shutdown complete");
}

void ThreadPoolExecutor::cancel_in_flight() {
    using Clock = std::chrono::steady_clock;
    constexpr auto NO_PROGRESS_GRACE = std::chrono::seconds(2);

    if (io_backend_) io_backend_->cancel_pending();

    std::size_t last_left = park_registry_.size();
    auto last_progress = Clock::now();
    for (;;) {
        park_registry_.cancel_all();
        quiesce();
        const std::size_t left = park_registry_.size();
        if (left == 0) return;
        if (left < last_left) {
            last_left = left;
            last_progress = Clock::now();
        } else if (Clock::now() - last_progress > NO_PROGRESS_GRACE) {
            DFTRACER_UTILS_LOG_WARN(
                "Executor shutdown: %zu suspended frames did not wake when "
                "cancelled and are abandoned",
                left);
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

// Runnable work finishes before the workers stop. A coroutine chain woken to
// unwind, such as a scan producer whose consumer stopped early, is destroyed
// mid-chain by the orphan sweep below otherwise, and every frame still
// awaiting it leaks. A worker in a blocking handoff counts as parked.
void ThreadPoolExecutor::quiesce() {
    for (;;) {
        if (live_workers_.load(std::memory_order_acquire) == 0) return;
        if (io_backend_) io_backend_->flush();
        bool busy = pending_tasks(true) != 0;
        if (!busy) {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            for (const auto& worker : workers_) {
                if (worker->thread.joinable() &&
                    !worker->is_idle.load(std::memory_order_seq_cst) &&
                    worker->blk_state.load(std::memory_order_acquire) ==
                        BLK_NONE) {
                    busy = true;
                    break;
                }
            }
            // A read still in flight resumes its coroutine when it lands.
            // Counted before the queue is read again: a completion enqueues
            // before it stops counting.
            busy = busy || (io_backend_ && io_backend_->in_flight() != 0) ||
                   pending_tasks(true) != 0;
        }
        if (!busy) return;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

bool ThreadPoolExecutor::spawn_worker(std::size_t cap, bool best_effort) {
    std::unique_lock<std::mutex> lock(workers_mutex_, std::defer_lock);
    if (best_effort) {
        if (!lock.try_lock()) return false;
    } else {
        lock.lock();
    }
    if (!running_.load(std::memory_order_acquire)) return false;
    if (live_workers_.load(std::memory_order_relaxed) >= cap) return false;
    auto worker = dstd::make_unique<WorkerContext>(
        next_worker_id_.fetch_add(1, std::memory_order_relaxed));
    worker->last_activity = std::chrono::steady_clock::now();
    worker->owner = this;
    worker->core = acquire_core();
    WorkerContext* ptr = worker.get();
    workers_.push_back(std::move(worker));
    live_workers_.fetch_add(1, std::memory_order_acq_rel);
    ptr->thread = dstd::Thread([this, ptr] { worker_thread(ptr); });
    return true;
}

void ThreadPoolExecutor::add_worker() {
    spawn_worker(thread_count_.load(std::memory_order_acquire),
                 /*best_effort=*/false);
}

void ThreadPoolExecutor::retire_worker() {
    dstd::UniquePtr<WorkerContext> victim;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        if (workers_.empty()) return;
        // Pull the target out of the vector under the lock, so wake/progress no
        // longer see it, then drive its exit outside the lock.
        victim = std::move(workers_.back());
        workers_.pop_back();
        live_workers_.fetch_sub(1, std::memory_order_acq_rel);
    }
    victim->retire.store(true, std::memory_order_release);
    wake_all_workers();  // wake it if parked so it observes retire
    if (victim->thread.joinable()) victim->thread.join();
    std::lock_guard<std::mutex> lock(workers_mutex_);
    free_cores_.push_back(victim->core);
    // victim (and its WorkerContext) is destroyed here, after the join.
}

bool ThreadPoolExecutor::acquire_permit(WorkerContext* ctx) {
    std::ptrdiff_t c = permits_.load(std::memory_order_acquire);
    for (;;) {
        while (c > 0) {
            if (permits_.compare_exchange_weak(c, c - 1,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                return true;
            }
        }
        if (!running_.load(std::memory_order_acquire) ||
            ctx->retire.load(std::memory_order_acquire)) {
            return false;
        }
        permit_waiters_.fetch_add(1, std::memory_order_acq_rel);
        // On the value read: a shrink can leave it below zero, and a wait on
        // zero would then return at once and spin.
        permits_.wait(c, std::memory_order_acquire);
        permit_waiters_.fetch_sub(1, std::memory_order_acq_rel);
        c = permits_.load(std::memory_order_acquire);
    }
}

void ThreadPoolExecutor::release_permit(WorkerContext* ctx) {
    if (!ctx->holds_permit) return;
    ctx->holds_permit = false;
    permits_.fetch_add(1, std::memory_order_acq_rel);
    permits_.notify_one();
}

void ThreadPoolExecutor::ensure_running_worker() {
    // A permit-waiter will take the freed slot; no need to spawn.
    if (permit_waiters_.load(std::memory_order_acquire) > 0) return;
    if (idle_workers_.load(std::memory_order_acquire) > 0 &&
        wake_sleeper(true)) {
        return;
    }
    const std::size_t justified =
        thread_count_.load(std::memory_order_acquire) +
        blocked_workers_.load(std::memory_order_acquire);
    if (live_workers_.load(std::memory_order_acquire) < justified) {
        spawn_worker(max_live_.load(std::memory_order_acquire),
                     /*best_effort=*/true);
    }
}

void ThreadPoolExecutor::enter_blocking() {
    auto* ctx = static_cast<WorkerContext*>(current_worker_context());
    if (!ctx || !ctx->holds_permit) return;
    // No thief takes from the LIFO slot and this thread stops running it.
    flush_lifo(*ctx->core);
    ctx->blocking_since_ns.store(common::steady_now_ns(),
                                 std::memory_order_relaxed);
    ctx->blk_state.store(BLK_SYSCALL, std::memory_order_release);
    // Only the 0->1 transition can find the sysmon parked; skip the wake else.
    // Notify under sysmon_mutex_ so the wake cannot slip between the sysmon's
    // predicate check and its wait().
    if (blocked_workers_.fetch_add(1, std::memory_order_acq_rel) == 0) {
        std::lock_guard<std::mutex> lock(sysmon_mutex_);
        sysmon_cv_.notify_one();
    }
}

void ThreadPoolExecutor::exit_blocking() {
    auto* ctx = static_cast<WorkerContext*>(current_worker_context());
    if (!ctx) return;
    blocked_workers_.fetch_sub(1, std::memory_order_acq_rel);
    int expected = BLK_SYSCALL;
    if (ctx->blk_state.compare_exchange_strong(expected, BLK_NONE,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        return;  // kept our permit
    }
    // Handed off: reacquire a permit before resuming.
    ctx->holds_permit = false;
    ctx->blk_state.store(BLK_NONE, std::memory_order_release);
    if (acquire_permit(ctx)) ctx->holds_permit = true;
}

void ThreadPoolExecutor::sysmon_loop() {
    while (running_.load(std::memory_order_acquire)) {
        {
            // Asleep only with nothing to watch. A worker that stops being idle
            // and one that enters a blocking region both wake it.
            std::unique_lock<std::mutex> lock(sysmon_mutex_);
            sysmon_parked_.store(true, std::memory_order_seq_cst);
            sysmon_cv_.wait(lock, [this] {
                return blocked_workers_.load(std::memory_order_acquire) > 0 ||
                       idle_workers_.load(std::memory_order_seq_cst) +
                               parked_drivers_.load(std::memory_order_seq_cst) <
                           live_workers_.load(std::memory_order_seq_cst) ||
                       !running_.load(std::memory_order_acquire);
            });
            sysmon_parked_.store(false, std::memory_order_relaxed);
        }
        if (!running_.load(std::memory_order_acquire)) break;

        check_lifo_stalls();
        if (blocked_workers_.load(std::memory_order_acquire) == 0) {
            std::this_thread::sleep_for(
                std::chrono::nanoseconds(LIFO_CHECK_NS));
            continue;
        }

        const std::int64_t now = common::steady_now_ns();
        int handed = 0;
        {
            std::unique_lock<std::mutex> wl(workers_mutex_, std::try_to_lock);
            if (wl.owns_lock()) {
                for (auto& w : workers_) {
                    if (w->blk_state.load(std::memory_order_acquire) !=
                        BLK_SYSCALL)
                        continue;
                    if (now - w->blocking_since_ns.load(
                                  std::memory_order_acquire) <
                        BLOCK_HANDOFF_NS)
                        continue;
                    int expected = BLK_SYSCALL;
                    if (w->blk_state.compare_exchange_strong(
                            expected, BLK_HANDED_OFF, std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        permits_.fetch_add(1, std::memory_order_acq_rel);
                        permits_.notify_one();
                        ++handed;
                    }
                }
            }
        }
        for (int i = 0; i < handed; ++i) ensure_running_worker();
        std::this_thread::sleep_for(std::chrono::nanoseconds(BLOCK_HANDOFF_NS));
    }
}

void ThreadPoolExecutor::check_lifo_stalls() noexcept {
    const std::size_t count = core_count_.load(std::memory_order_acquire);
    Core* const* list = core_list_.load(std::memory_order_acquire);
    bool moved = false;
    for (std::size_t i = 0; i < count; ++i) {
        Core& core = *list[i];
        void* task = core.lifo.load(std::memory_order_acquire);
        if (task != nullptr && task == core.lifo_seen) {
            lifo_in_transit_.fetch_add(1, std::memory_order_seq_cst);
            if (core.lifo.compare_exchange_strong(task, nullptr,
                                                  std::memory_order_acq_rel)) {
                inject_.push(task);
                moved = true;
                task = nullptr;
            }
            lifo_in_transit_.fetch_sub(1, std::memory_order_seq_cst);
        }
        core.lifo_seen = task;
    }
    if (moved) notify();
}

void ThreadPoolExecutor::wake_sysmon_if_parked() {
    if (!sysmon_parked_.load(std::memory_order_seq_cst)) return;
    std::lock_guard<std::mutex> lock(sysmon_mutex_);
    sysmon_cv_.notify_one();
}

void ThreadPoolExecutor::set_threads(std::size_t threads,
                                     dstd::Optional<bool> eager) {
    if (threads == 0) threads = common::hardware_concurrency();
    if (threads == 0) threads = 2;
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    if (threads > 2) threads = 2;
#endif
    std::lock_guard<std::mutex> resize(resize_mutex_);
    const std::size_t old = thread_count_.load(std::memory_order_acquire);
    const bool was_eager = eager_.load(std::memory_order_acquire);
    const bool now_eager = eager.value_or(was_eager);
    eager_.store(now_eager, std::memory_order_release);
    if (io_backend_)
        io_backend_->grow_threads(io_pool_size_.load(std::memory_order_acquire),
                                  now_eager);
    // An elastic pool keeps its floor. One that turns elastic starts at one.
    const std::size_t old_floor = min_workers_.load(std::memory_order_acquire);
    const std::size_t floor =
        now_eager ? threads
                  : std::min(was_eager ? std::size_t{1} : old_floor, threads);
    // The floor first when shrinking and the cap first when growing, so the
    // monitor never sees a floor above the cap.
    if (threads < old) min_workers_.store(floor, std::memory_order_release);
    thread_count_.store(threads, std::memory_order_release);
    max_live_.store(threads + MAX_BLOCKING_REPLACEMENTS,
                    std::memory_order_release);
    min_workers_.store(floor, std::memory_order_release);
    // A shrink leaves permits below zero until running workers release them.
    permits_.fetch_add(
        static_cast<std::ptrdiff_t>(threads) - static_cast<std::ptrdiff_t>(old),
        std::memory_order_acq_rel);
    if (threads > old) permits_.notify_all();
    if (!running_.load(std::memory_order_acquire)) return;
    if (floor < threads && !monitor_thread_.joinable()) {
        monitor_thread_ = dstd::Thread([this] { monitor_loop(); });
    }
    while (live_workers_.load(std::memory_order_acquire) < floor &&
           spawn_worker(floor, /*best_effort=*/false)) {
    }
    DFTRACER_UTILS_LOG_DEBUG("Executor threads %zu -> %zu (floor %zu)", old,
                             threads, floor);
}

void ThreadPoolExecutor::grow_io_threads(std::size_t threads) {
    std::lock_guard<std::mutex> resize(resize_mutex_);
    const std::size_t cap =
        std::max(threads, io_pool_size_.load(std::memory_order_acquire));
    if (io_backend_ &&
        io_backend_->grow_threads(cap, eager_.load(std::memory_order_acquire)))
        io_pool_size_.store(cap, std::memory_order_release);
}

void ThreadPoolExecutor::set_deadlock_timeout(std::chrono::seconds timeout) {
    deadlock_timeout_s_.store(timeout.count(), std::memory_order_relaxed);
}

void ThreadPoolExecutor::monitor_loop() {
    using Clock = std::chrono::steady_clock;
    const auto keepalive = keepalive_;
    auto last_busy = Clock::now();

    std::unique_lock<std::mutex> lock(coord_mutex_);
    while (running_.load(std::memory_order_acquire)) {
        // Park until enqueue signals unmet demand (grow) or a keep-alive
        // elapses (shrink check). No busy-poll.
        coord_cv_.wait_for(lock, keepalive, [this] {
            return grow_pending_ || !running_.load(std::memory_order_acquire);
        });
        grow_pending_ = false;
        lock.unlock();  // add/retire take workers_mutex_, not coord_mutex_
        if (!running_.load(std::memory_order_acquire)) {
            lock.lock();
            break;
        }

        if (pending_tasks(false) > 0 ||
            idle_workers_.load(std::memory_order_acquire) <
                live_workers_.load(std::memory_order_acquire)) {
            last_busy = Clock::now();
        }
        // Grow: spawn until the backlog is covered or we hit the cap - all at
        // once, so a burst reaches full parallelism in one wake.
        while (pending_tasks(false) > 0 &&
               live_workers_.load(std::memory_order_acquire) <
                   thread_count_.load(std::memory_order_acquire)) {
            add_worker();
        }
        // Shrink after a keep-alive of sustained idle, by half the workers past
        // the floor each time, so a brief lull costs nothing and a pool of any
        // size reaches its floor in a few keep-alives.
        const std::size_t live = live_workers_.load(std::memory_order_acquire);
        const std::size_t floor = min_workers_.load(std::memory_order_acquire);
        if (live > floor && Clock::now() - last_busy >= keepalive) {
            for (std::size_t n = std::max<std::size_t>((live - floor) / 2, 1);
                 n > 0; --n)
                retire_worker();
            last_busy = Clock::now();
        }
        lock.lock();
    }
}

void ThreadPoolExecutor::worker_thread(WorkerContext* context) {
    DFTRACER_UTILS_LOG_DEBUG("Worker %zu started", context->worker_id);

    set_current_worker_context(context);
    coro::CurrentScope current_scope{*this};
    set_current_backend(this);
    coro::reset_timeslice();
    wake_sysmon_if_parked();

    memory::Pool& general = memory::detail::Manager::get().general();
    Core& core = *context->core;

    // Fixed for the process lifetime; read once to keep the resume loop cheap.
    const bool monitor = coro::detail::monitoring_enabled();
    if (monitor) {
        coro::detail::monitor_set_worker(static_cast<int>(context->worker_id));
    }

    bool marked_idle = false;
    while (running_ && !context->retire.load(std::memory_order_acquire)) {
        // A cap that shrank leaves the count below zero, so a holder gives its
        // permit back before its next task.
        if (context->holds_permit &&
            permits_.load(std::memory_order_relaxed) < 0) {
            flush_lifo(core);
            release_permit(context);
        }
        // Hold a permit to run; a surplus thread (live > cap) parks for a slot.
        if (!context->holds_permit) {
            if (!acquire_permit(context)) break;
            context->holds_permit = true;
        }

        // Busy before the dequeue, so quiesce() never sees an empty queue and
        // an idle worker while this worker holds a task.
        if (marked_idle) {
            context->is_idle.store(false, std::memory_order_seq_cst);
            marked_idle = false;
        }

        void* task = next_task(core);
        if (task == nullptr) {
            if (io_backend_) {
                io_backend_->flush();
                // A flush can complete an op inline, which posts to this core.
                if (io_backend_->poll(0) > 0 ||
                    core.lifo.load(std::memory_order_relaxed) != nullptr ||
                    core.queue.size_approx() != 0) {
                    continue;
                }
            }
            if (try_start_searching(context)) task = steal_task(core);
            if (task == nullptr) {
                if (!running_.load(std::memory_order_acquire) ||
                    context->retire.load(std::memory_order_acquire)) {
                    break;
                }
                marked_idle = true;
                park_worker(context);
                continue;
            }
        }
        if (context->searching) stop_searching(context);

        coro::reset_timeslice();
        context->resumes.store(
            context->resumes.load(std::memory_order_relaxed) + 1,
            std::memory_order_relaxed);
        const auto handle = std::coroutine_handle<>::from_address(task);
        if (!handle.done()) {
            DFTRACER_UTILS_TSAN_ACQUIRE(task);
            if (monitor) {
                coro::detail::monitor_resume_begin(
                    coro::detail::monitor_enqueue(
                        task, coro::detail::CoroKind::Spawn));
            }
            {
                // Our coroutines enter their own pool. Anything else charges
                // the general pool.
                memory::detail::Scope scope{general, nullptr};
                coro::resume_on(*this, handle);
            }
            if (monitor) {
                coro::detail::monitor_resume_end(task, handle.done());
            }
        }
        // A batch held until this worker idles would wait behind a later slice
        // that blocks, so ops submitted in this slice start now.
        if (io_backend_) io_backend_->flush();
        // Destroy coroutine frames that FinalAwaiter deferred to this thread.
        // Safe: resume() has fully returned, so the frame is suspended at
        // final_suspend and no code references it.
        coro::drain_thread_local_destroys();
    }

    if (context->searching) {
        context->searching = false;
        searching_.fetch_sub(1, std::memory_order_acq_rel);
    }

    // Release the permit if we broke out of the loop still holding it.
    release_permit(context);

    // Final drain: destroy any frames deferred during the last resume().
    coro::drain_thread_local_destroys();

    flush_core(core);
    // A task moved here while the other workers park has no one to run it.
    notify();

    set_current_backend(nullptr);
    set_current_worker_context(nullptr);

    DFTRACER_UTILS_LOG_DEBUG("Worker %zu terminated", context->worker_id);
}

bool ThreadPoolExecutor::drive_until(
    dftracer::utils::dstd::FunctionRef<bool()> done) {
    // Only a worker has a core. @ref coro::drive_to_completion calls this on
    // the executor of the calling thread, which for this pool is a worker.
    WorkerContext* ctx = owned_context();
    if (ctx == nullptr) return false;
    Core& core = *ctx->core;
    memory::Pool& general = memory::detail::Manager::get().general();
    // The timeout re-checks `done` for a predicate that nobody wakes.
    std::chrono::milliseconds park = IDLE_PARK_MIN;

    while (!done()) {
        void* task = next_task(core);
        if (task == nullptr) task = steal_task(core);
        if (task != nullptr) {
            coro::reset_timeslice();
            const auto handle = std::coroutine_handle<>::from_address(task);
            if (!handle.done()) {
                DFTRACER_UTILS_TSAN_ACQUIRE(task);
                memory::detail::Scope scope{general, nullptr};
                coro::resume_on(*this, handle);
            }
            coro::drain_thread_local_destroys();
            park = IDLE_PARK_MIN;
            continue;
        }

        if (done()) break;
        if (!running_.load(std::memory_order_acquire)) break;

        if (io_backend_) {
            io_backend_->flush();
            if (io_backend_->poll(0) > 0 ||
                core.lifo.load(std::memory_order_relaxed) != nullptr ||
                core.queue.size_approx() != 0) {
                park = IDLE_PARK_MIN;
                continue;
            }
        }
        struct Wait {
            dftracer::utils::dstd::FunctionRef<bool()> done;
            const ThreadPoolExecutor& self;
        } wait{done, *this};
        parked_drivers_.fetch_add(1, std::memory_order_seq_cst);
        const bool woke = coro::park_driver(
            [](void* arg) {
                const auto& w = *static_cast<const Wait*>(arg);
                return w.done() ||
                       !w.self.running_.load(std::memory_order_acquire) ||
                       w.self.pending_tasks(false) != 0;
            },
            &wait, park);
        parked_drivers_.fetch_sub(1, std::memory_order_seq_cst);
        wake_sysmon_if_parked();
        park = woke ? IDLE_PARK_MIN : std::min(park * 2, IDLE_PARK_MAX);
    }
    coro::drain_thread_local_destroys();
    return true;
}

ThreadPoolExecutor::WorkerContext* ThreadPoolExecutor::owned_context()
    const noexcept {
    auto* ctx = static_cast<WorkerContext*>(current_worker_context());
    return ctx != nullptr && ctx->owner == this ? ctx : nullptr;
}

ThreadPoolExecutor::Core* ThreadPoolExecutor::acquire_core() {
    if (!free_cores_.empty()) {
        Core* core = free_cores_.back();
        free_cores_.pop_back();
        return core;
    }
    // Lives as long as the pool, past the pool scope of whoever made it grow.
    memory::detail::Scope bookkeeping{memory::detail::Manager::get().general(),
                                      nullptr};
    auto core = dstd::make_unique<Core>();
    core->steal_seed = static_cast<std::uint32_t>(cores_.size() + 1) *
                       std::uint32_t{0x9E3779B1};
    const std::size_t count = core_count_.load(std::memory_order_relaxed);
    if (core_lists_.empty() || count == core_lists_.back().size()) {
        dstd::Vector<Core*> larger(std::max(CORE_LIST_MIN, count * 2));
        for (std::size_t i = 0; i < count; ++i) {
            larger[i] = core_lists_.back()[i];
        }
        core_lists_.push_back(std::move(larger));
        core_list_.store(core_lists_.back().data(), std::memory_order_release);
    }
    core_lists_.back()[count] = core.get();
    core_count_.store(count + 1, std::memory_order_release);
    cores_.push_back(std::move(core));
    return cores_.back().get();
}

void* ThreadPoolExecutor::take_injected(Core& core) {
    const std::size_t waiting = inject_.size_approx();
    if (waiting == 0) return nullptr;
    const std::size_t live =
        std::max<std::size_t>(live_workers_.load(std::memory_order_relaxed), 1);
    void* batch[INJECT_BATCH_MAX];
    const std::size_t count = inject_.pop_batch(
        batch, std::min(waiting / live + 1, INJECT_BATCH_MAX));
    if (count == 0) return nullptr;
    for (std::size_t i = 1; i < count; ++i) {
        core.queue.push_back(batch[i], inject_);
    }
    if (count > 1) notify();
    return batch[0];
}

void* ThreadPoolExecutor::next_task(Core& core) {
    if (++core.tick % INJECT_CHECK_INTERVAL == 0) {
        if (void* task = take_injected(core)) {
            core.lifo_streak = 0;
            return task;
        }
    }
    if (core.lifo_streak < LIFO_STREAK_MAX &&
        core.lifo.load(std::memory_order_relaxed) != nullptr) {
        if (void* task =
                core.lifo.exchange(nullptr, std::memory_order_acq_rel)) {
            ++core.lifo_streak;
            return task;
        }
    }
    core.lifo_streak = 0;
    if (void* task = core.queue.pop()) return task;
    if (void* task = take_injected(core)) return task;
    if (core.lifo.load(std::memory_order_relaxed) != nullptr) {
        return core.lifo.exchange(nullptr, std::memory_order_acq_rel);
    }
    return nullptr;
}

void* ThreadPoolExecutor::steal_task(Core& core) {
    const std::size_t count = core_count_.load(std::memory_order_acquire);
    Core* const* list = core_list_.load(std::memory_order_acquire);
    // xorshift32: a start point only, to spread thieves over the victims.
    std::uint32_t seed = core.steal_seed;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    core.steal_seed = seed;
    for (int round = 0; round < STEAL_ROUNDS; ++round) {
        for (std::size_t i = 0; i < count; ++i) {
            Core* victim = list[(seed + i) % count];
            if (victim == &core) continue;
            if (void* task = victim->queue.steal_into(core.queue)) return task;
        }
        if (void* task = take_injected(core)) return task;
    }
    return nullptr;
}

bool ThreadPoolExecutor::try_start_searching(WorkerContext* ctx) noexcept {
    if (ctx->searching) return true;
    // Half the pool searching finds work as fast as all of it and keeps the
    // other half from hammering the same queues.
    if (2 * searching_.load(std::memory_order_relaxed) >=
        live_workers_.load(std::memory_order_relaxed)) {
        return false;
    }
    ctx->searching = true;
    searching_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void ThreadPoolExecutor::stop_searching(WorkerContext* ctx) noexcept {
    ctx->searching = false;
    // The last searcher found work, so there may be more: pass the search on.
    if (searching_.fetch_sub(1, std::memory_order_acq_rel) == 1) notify();
}

void ThreadPoolExecutor::flush_lifo(Core& core) {
    if (core.lifo.load(std::memory_order_relaxed) == nullptr) return;
    if (void* task = core.lifo.exchange(nullptr, std::memory_order_acq_rel)) {
        core.queue.push_back(task, inject_);
        notify();
    }
}

void ThreadPoolExecutor::flush_core(Core& core) {
    if (void* task = core.lifo.exchange(nullptr, std::memory_order_acq_rel)) {
        inject_.push(task);
    }
    while (void* task = core.queue.pop()) inject_.push(task);
}

std::size_t ThreadPoolExecutor::pending_tasks(bool with_lifo) const noexcept {
    // In the order a task from a LIFO slot moves, so a move cannot pass the
    // reads unseen.
    std::size_t total = 0;
    const std::size_t count = core_count_.load(std::memory_order_acquire);
    Core* const* list = core_list_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        total += list[i]->queue.size_approx();
        if (with_lifo && list[i]->lifo.load(std::memory_order_seq_cst)) ++total;
    }
    if (with_lifo) total += lifo_in_transit_.load(std::memory_order_seq_cst);
    return total + inject_.size_approx();
}

void ThreadPoolExecutor::park_worker(WorkerContext* ctx) {
    // Give up the permit for a waiting/surplus thread, then park.
    release_permit(ctx);
    ctx->is_idle.store(true, std::memory_order_seq_cst);
    if (ctx->searching) {
        ctx->searching = false;
        searching_.fetch_sub(1, std::memory_order_acq_rel);
    }
    idle_workers_.fetch_add(1, std::memory_order_seq_cst);
    {
        memory::detail::Scope bookkeeping{
            memory::detail::Manager::get().general(), nullptr};
        std::lock_guard<std::mutex> lock(sleepers_mutex_);
        ctx->notified = false;
        sleepers_.push_back(ctx);
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const bool work = pending_tasks(false) != 0;

    bool timed_out = false;
    {
        std::unique_lock<std::mutex> lock(sleepers_mutex_);
        const auto keep_waiting = [&] {
            return !work && !ctx->notified &&
                   running_.load(std::memory_order_acquire) &&
                   !ctx->retire.load(std::memory_order_acquire);
        };
        while (keep_waiting()) {
            if (ctx->park_cv.wait_for(lock, IDLE_PARK_MAX) ==
                std::cv_status::timeout) {
                timed_out = true;
                break;
            }
        }
        if (ctx->notified) {
            ctx->searching = true;
        } else {
            auto it = std::find(sleepers_.begin(), sleepers_.end(), ctx);
            if (it != sleepers_.end()) sleepers_.erase(it);
        }
    }
    idle_workers_.fetch_sub(1, std::memory_order_seq_cst);
    wake_sysmon_if_parked();
    // Only this thread may purge its heap, so an idle worker gives its freed
    // pages back itself.
    if (timed_out) memory::detail::trim_thread();
}

bool ThreadPoolExecutor::wake_sleeper(bool force) noexcept {
    std::lock_guard<std::mutex> lock(sleepers_mutex_);
    if (sleepers_.empty()) return false;
    if (!force && searching_.load(std::memory_order_relaxed) > 0) return false;
    WorkerContext* ctx = sleepers_.back();
    sleepers_.pop_back();
    ctx->notified = true;
    searching_.fetch_add(1, std::memory_order_acq_rel);
    // Under the lock: past it the worker can exit and its context can go.
    ctx->park_cv.notify_one();
    return true;
}

void ThreadPoolExecutor::notify() noexcept {
    // Pairs with the fence in park_worker.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    coro::wake_parked_drivers();
    if (searching_.load(std::memory_order_relaxed) > 0) return;
    if (idle_workers_.load(std::memory_order_relaxed) > 0) {
        wake_sleeper(false);
        return;
    }
    const std::size_t cap = thread_count_.load(std::memory_order_relaxed);
    if (min_workers_.load(std::memory_order_relaxed) >= cap ||
        live_workers_.load(std::memory_order_relaxed) >= cap) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(coord_mutex_);
        grow_pending_ = true;
    }
    coord_cv_.notify_one();
}

void ThreadPoolExecutor::wake_all_workers() {
    {
        std::lock_guard<std::mutex> lock(sleepers_mutex_);
        for (WorkerContext* ctx : sleepers_) {
            ctx->notified = true;
            searching_.fetch_add(1, std::memory_order_acq_rel);
            ctx->park_cv.notify_one();
        }
        sleepers_.clear();
    }
    coro::wake_parked_drivers();
    // Wake workers parked in acquire_permit so they re-check running_/retire.
    permits_.notify_all();
}

void ThreadPoolExecutor::schedule_foreign(void* task) noexcept {
    foreign_enqueues_.fetch_add(1, std::memory_order_acq_rel);
    inject_.push(task);
    notify();
    foreign_enqueues_.fetch_sub(1, std::memory_order_release);
}

void ThreadPoolExecutor::post(std::coroutine_handle<> handle) noexcept {
    if (!handle || handle.done()) return;
    if (coro::detail::monitoring_enabled()) {
        coro::detail::monitor_enqueue(handle.address(),
                                      coro::detail::CoroKind::Spawn);
    }
    void* task = handle.address();
    DFTRACER_UTILS_TSAN_RELEASE(task);
    WorkerContext* ctx = owned_context();
    if (ctx == nullptr) {
        schedule_foreign(task);
        return;
    }
    Core& core = *ctx->core;
    // Taken from a slot that was empty, nothing new is stealable, and this
    // worker runs the task as soon as the running one suspends.
    void* displaced = core.lifo.exchange(task, std::memory_order_acq_rel);
    if (displaced == nullptr) return;
    core.queue.push_back(displaced, inject_);
    notify();
}

void ThreadPoolExecutor::post_yield(std::coroutine_handle<> handle) noexcept {
    if (!handle || handle.done()) return;
    void* task = handle.address();
    DFTRACER_UTILS_TSAN_RELEASE(task);
    WorkerContext* ctx = owned_context();
    if (ctx == nullptr) {
        schedule_foreign(task);
        return;
    }
    ctx->core->queue.push_back(task, inject_);
    notify();
}

bool ThreadPoolExecutor::is_responsive() const {
    if (!running_.load()) {
        return false;
    }

    // Deadlocked means work is pending and no worker has resumed anything for
    // deadlock_timeout_. A task may run for hours while its coroutines resume.
    std::uint64_t resumes = 0;
    bool busy = pending_tasks(true) > 0;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        for (const auto& worker : workers_) {
            resumes += worker->resumes.load(std::memory_order_relaxed);
            busy = busy || !worker->is_idle.load(std::memory_order_relaxed);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    const auto now_ns = now.time_since_epoch().count();
    if (!busy || seen_resumes_.exchange(resumes) != resumes) {
        last_progress_ns_.store(now_ns, std::memory_order_relaxed);
        return true;
    }

    const auto stalled =
        now - std::chrono::steady_clock::time_point(
                  std::chrono::steady_clock::duration(
                      last_progress_ns_.load(std::memory_order_relaxed)));
    if (stalled > std::chrono::seconds(
                      deadlock_timeout_s_.load(std::memory_order_relaxed))) {
        DFTRACER_UTILS_LOG_WARN(
            "Executor appears deadlocked: %zu threads, work pending, nothing "
            "resumed for %lld ms",
            thread_count_.load(std::memory_order_relaxed),
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(stalled)
                    .count()));
        return false;
    }
    return true;
}

ExecutorProgress ThreadPoolExecutor::progress() const {
    std::shared_lock<std::shared_mutex> lock(registry_mutex_);
    ExecutorProgress progress;

    // Overall stats
    progress.total_tasks_submitted = total_tasks_submitted_.load();
    progress.tasks_completed = tasks_completed_.load();

    // Count task states
    progress.tasks_queued = 0;
    progress.tasks_running = 0;
    progress.tasks_failed = tasks_failed_.load();

    for (const auto& [task_id, info] : task_registry_) {
        switch (info.state) {
            case TaskState::Queued:
                progress.tasks_queued++;
                break;
            case TaskState::Running:
            case TaskState::Waiting:
                progress.tasks_running++;
                break;
            case TaskState::Completed:
                // Already counted
                break;
            case TaskState::Failed:
                break;
        }

        if (info.state == TaskState::Failed && !info.error_message.empty()) {
            progress.recent_errors.push_back({task_id, info.error_message});
        }
    }

    for (const auto& [task_id, info] : task_registry_) {
        progress.root_tasks.push_back(task_progress_of(info));
    }

    // Worker states (dynamic set; lock against add/retire mutation)
    {
        std::lock_guard<std::mutex> workers_lock(workers_mutex_);
        for (const auto& worker : workers_) {
            ExecutorProgress::WorkerStatus status;
            status.worker_id = worker->worker_id;
            status.is_idle = worker->is_idle.load();

            common::TaskIndex current_id = worker->current_task_id.load();
            if (current_id != -1) {
                status.current_task_id = current_id;
                std::lock_guard<std::mutex> name_lock(worker->task_name_mutex);
                status.current_task_name = worker->current_task_name;
            }

            status.local_queue_depth = worker->core->queue.size_approx();

            progress.workers.push_back(status);
            progress.worker_queue_depths.push_back(status.local_queue_depth);
        }
    }

    return progress;
}

TaskProgress ThreadPoolExecutor::task_progress_of(const TaskInfo& info) const {
    TaskProgress progress;
    progress.task_id = info.task_id;
    progress.name = info.name;

    // State
    progress.state = info.state;

    // Timing
    auto now = std::chrono::steady_clock::now();
    if (info.state == TaskState::Queued) {
        progress.queued_duration_ms =
            std::chrono::duration<double, std::milli>(now - info.queued_at)
                .count();
        progress.execution_duration_ms = 0;
    } else if (info.state == TaskState::Running ||
               info.state == TaskState::Waiting) {
        progress.queued_duration_ms = std::chrono::duration<double, std::milli>(
                                          info.started_at - info.queued_at)
                                          .count();
        progress.execution_duration_ms =
            std::chrono::duration<double, std::milli>(now - info.started_at)
                .count();
    } else {  // COMPLETED or FAILED
        progress.queued_duration_ms = std::chrono::duration<double, std::milli>(
                                          info.started_at - info.queued_at)
                                          .count();
        progress.execution_duration_ms =
            std::chrono::duration<double, std::milli>(info.completed_at -
                                                      info.started_at)
                .count();
    }

    progress.progress_percentage =
        (info.state == TaskState::Completed) ? 100.0 : 0.0;

    progress.location = info.location;
    progress.worker = (info.location == TaskLocation::LocalQueue ||
                       info.location == TaskLocation::Executing)
                          ? static_cast<std::int32_t>(info.worker_id)
                          : -1;

    return progress;
}

// ============================================================================
// Phase 3: Coro-based task execution
// ============================================================================

common::TaskIndex ThreadPoolExecutor::enqueue_tracked(
    coro::detail::Coro coro, dstd::StringView name,
    dstd::SharedPtr<std::atomic<common::TaskIndex>> tid_out) {
    common::TaskIndex id =
        next_coro_task_id_.fetch_sub(1, std::memory_order_relaxed);
    coro.handle().promise().executor = this;

    // The registry and tracking nodes outlive the submitting thread's pool
    // scope, so they charge the general pool.
    memory::detail::Scope bookkeeping_scope{
        memory::detail::Manager::get().general(), nullptr};
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        auto [it, inserted] = task_registry_.emplace(std::piecewise_construct,
                                                     std::forward_as_tuple(id),
                                                     std::forward_as_tuple());
        if (inserted) {
            auto now = std::chrono::steady_clock::now();
            it->second.task_id = id;
            it->second.name.assign(name.data(), name.size());
            it->second.state = TaskState::Queued;
            it->second.queued_at = now;
            it->second.location = TaskLocation::SharedQueue;
            it->second.worker_id = static_cast<std::size_t>(-1);
        }
        coro.handle().promise().tracking =
            &trackings_
                 .emplace(id,
                          coro::detail::TaskTracking{
                              &ThreadPoolExecutor::on_tracked_task_complete,
                              this, id})
                 .first->second;
    }
    ++total_tasks_submitted_;

    if (tid_out) {
        tid_out->store(id, std::memory_order_release);
    }

    auto handle = coro.release();
    if (coro::detail::monitoring_enabled()) {
        coro::detail::monitor_enqueue(handle.address(),
                                      coro::detail::CoroKind::Task);
    }
    post(handle);
    return id;
}

common::TaskIndex ThreadPoolExecutor::current_worker_task_id() const {
    auto* ctx = static_cast<WorkerContext*>(current_worker_context());
    return ctx ? ctx->current_task_id.load() : -1;
}

void ThreadPoolExecutor::mark_coro_started(common::TaskIndex id) {
    const WorkerContext* ctx = owned_context();
    std::unique_lock<std::shared_mutex> lock(registry_mutex_);
    auto it = task_registry_.find(id);
    if (it == task_registry_.end() || it->second.state != TaskState::Queued) {
        return;
    }
    it->second.state = TaskState::Running;
    it->second.started_at = std::chrono::steady_clock::now();
    it->second.location = TaskLocation::Executing;
    if (ctx != nullptr) it->second.worker_id = ctx->worker_id;
}

void ThreadPoolExecutor::mark_coro_completed(common::TaskIndex id) {
    bool was_new = false;
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        auto it = task_registry_.find(id);
        if (it != task_registry_.end() &&
            it->second.state != TaskState::Completed &&
            it->second.state != TaskState::Failed) {
            auto now = std::chrono::steady_clock::now();
            if (it->second.started_at.time_since_epoch().count() == 0) {
                it->second.started_at = now;
            }
            it->second.state = TaskState::Completed;
            it->second.completed_at = now;
            it->second.location = TaskLocation::Done;
            was_new = true;
            retire_locked(id);
        }
    }
    if (was_new) ++tasks_completed_;
}

void ThreadPoolExecutor::mark_coro_failed(common::TaskIndex id,
                                          dstd::StringView message) {
    bool was_new = false;
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        auto it = task_registry_.find(id);
        if (it != task_registry_.end() &&
            it->second.state != TaskState::Completed &&
            it->second.state != TaskState::Failed) {
            auto now = std::chrono::steady_clock::now();
            if (it->second.started_at.time_since_epoch().count() == 0) {
                it->second.started_at = now;
            }
            it->second.state = TaskState::Failed;
            it->second.completed_at = now;
            it->second.error_message.assign(message.data(), message.size());
            it->second.location = TaskLocation::Done;
            was_new = true;
            retire_locked(id);
        }
    }
    if (was_new) ++tasks_failed_;
}

void ThreadPoolExecutor::retire_locked(common::TaskIndex id) {
    // The history outlives the task's pool scope, so it charges the general
    // pool.
    memory::detail::Scope history_scope{
        memory::detail::Manager::get().general()};
    finished_.push_back(id);
    while (finished_.size() > task_history_) {
        task_registry_.erase(finished_.front());
        finished_.pop_front();
    }
}

void ThreadPoolExecutor::on_tracked_task_complete(
    void* owner, common::TaskIndex id) noexcept {
    auto* self = static_cast<ThreadPoolExecutor*>(owner);
    self->mark_coro_completed(id);
    // The caller copied the owner and id before this call and does not read
    // the tracking again, so it can go. A wrapper that calls
    // mark_coro_completed itself still needs it until its final suspend.
    std::unique_lock<std::shared_mutex> lock(self->registry_mutex_);
    self->trackings_.erase(id);
}

}  // namespace dftracer::utils
