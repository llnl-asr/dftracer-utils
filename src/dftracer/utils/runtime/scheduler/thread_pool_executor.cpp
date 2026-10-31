#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/common/timer.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/monitor.h>
#include <dftracer/utils/runtime/coro/yield.h>
#include <dftracer/utils/runtime/io/io_backend_factory.h>
#include <dftracer/utils/runtime/scheduler/thread_pool_executor.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/runtime/tasks/error.h>
#include <dftracer/utils/runtime/tasks/task.h>

#include <chrono>
#include <coroutine>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace dftracer::utils {

ThreadPoolExecutor::ThreadPoolExecutor(const ExecutorConfig& config)
    : last_progress_ns_(steady_now_ns()),
      idle_timeout_(config.idle_timeout),
      deadlock_timeout_s_(config.deadlock_timeout.count()),
      keepalive_(config.elastic_keepalive),
      io_backend_type_(config.io_backend_type),
      io_batch_threshold_(config.io_batch_threshold) {
    std::size_t threads =
        config.num_threads == 0 ? hardware_concurrency() : config.num_threads;
    if (threads == 0) {
        threads = 2;
    }
    std::size_t io_threads =
        config.io_pool_size == 0 ? hardware_concurrency() : config.io_pool_size;
    if (io_threads == 0) {
        io_threads = 2;
    }
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    // Per-Executor thread churn dominates runtime when Valgrind serializes and
    // instruments every thread, so cap the pools.
    if (threads > 2) threads = 2;
    if (io_threads > 2) io_threads = 2;
#endif
    io_pool_size_.store(io_threads, std::memory_order_relaxed);
    num_threads_.store(threads, std::memory_order_relaxed);
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
    drain_destroy_queue();
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
    const std::size_t threads = num_threads_.load(std::memory_order_acquire);
    const std::size_t floor = min_workers_.load(std::memory_order_acquire);
    workers_.clear();
    workers_.reserve(threads);
    permits_.store(static_cast<std::ptrdiff_t>(threads),
                   std::memory_order_release);

    timer_service_.start();

    // Create and start I/O backend before workers so workers
    // can use it immediately.
    io_backend_ = io::create_io_backend(*this, io_pool_size_.load(),
                                        io_backend_type_, io_batch_threshold_);
    io_backend_->start();

    // Spawn the floor (== the cap in eager mode). The elastic monitor grows the
    // rest on demand. Contexts first so workers_ is stable before any worker
    // thread can iterate it.
    for (std::size_t i = 0; i < floor; ++i) {
        auto worker = std::make_unique<WorkerContext>(
            next_worker_id_.fetch_add(1, std::memory_order_relaxed));
        worker->last_activity = std::chrono::steady_clock::now();
        workers_.push_back(std::move(worker));
    }
    live_workers_.store(workers_.size(), std::memory_order_release);

    // Start worker threads after all contexts are in place.
    for (auto& worker : workers_) {
        worker->thread =
            std::thread(&ThreadPoolExecutor::worker_thread, this, worker.get());
    }

    // Elastic mode only: a monitor drives grow/shrink between the floor and
    // cap.
    if (floor < threads) {
        monitor_thread_ = std::thread(&ThreadPoolExecutor::monitor_loop, this);
    }

    // Reclaims permits from long blocks; blocking can occur in any mode.
    sysmon_thread_ = std::thread(&ThreadPoolExecutor::sysmon_loop, this);

    DFTRACER_UTILS_LOG_DEBUG(
        "Executor started with %zu worker threads (cap %zu)", floor, threads);
}

void ThreadPoolExecutor::shutdown() {
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

    drain_destroy_queue();
    {
        RunQueueEntry orphan;
        while (run_queue_.try_dequeue(orphan)) {
            if (orphan.handle) {
                orphan.handle.destroy();
            }
        }
    }

    {
        std::lock_guard<std::mutex> workers_lock(workers_mutex_);
        workers_.clear();
    }
    live_workers_.store(0, std::memory_order_release);
    timer_service_.stop();

    // Drain the main thread's thread-local destroy list (for
    // coroutines whose FinalAwaiter ran on the main thread).
    drain_thread_local_destroys();

    DFTRACER_UTILS_LOG_DEBUG("%s", "Executor shutdown complete");
}

void ThreadPoolExecutor::cancel_in_flight() {
    using Clock = std::chrono::steady_clock;
    constexpr auto NO_PROGRESS_GRACE = std::chrono::seconds(2);

    shutdown_token_->store(true, std::memory_order_release);
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
        bool busy = run_queue_.size_approx() != 0;
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
                   run_queue_.size_approx() != 0;
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
    auto worker = std::make_unique<WorkerContext>(
        next_worker_id_.fetch_add(1, std::memory_order_relaxed));
    worker->last_activity = std::chrono::steady_clock::now();
    WorkerContext* ptr = worker.get();
    workers_.push_back(std::move(worker));
    live_workers_.fetch_add(1, std::memory_order_acq_rel);
    ptr->thread = std::thread(&ThreadPoolExecutor::worker_thread, this, ptr);
    return true;
}

void ThreadPoolExecutor::add_worker() {
    spawn_worker(num_threads_.load(std::memory_order_acquire),
                 /*best_effort=*/false);
}

void ThreadPoolExecutor::retire_worker() {
    std::unique_ptr<WorkerContext> victim;
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
    if (idle_workers_.load(std::memory_order_acquire) > 0) {
        wake_one_worker();
        return;
    }
    const std::size_t justified =
        num_threads_.load(std::memory_order_acquire) +
        blocked_workers_.load(std::memory_order_acquire);
    if (live_workers_.load(std::memory_order_acquire) < justified) {
        spawn_worker(max_live_.load(std::memory_order_acquire),
                     /*best_effort=*/true);
    }
}

void ThreadPoolExecutor::enter_blocking() {
    auto* ctx = static_cast<WorkerContext*>(get_current_worker_context());
    if (!ctx || !ctx->holds_permit) return;
    ctx->blocking_since_ns.store(steady_now_ns(), std::memory_order_relaxed);
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
    auto* ctx = static_cast<WorkerContext*>(get_current_worker_context());
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
            std::unique_lock<std::mutex> lock(sysmon_mutex_);
            if (blocked_workers_.load(std::memory_order_acquire) == 0) {
                // Park until a worker enters a blocking region or we shut down;
                // both notify under sysmon_mutex_, so there is no wakeup to
                // miss and no need to poll.
                sysmon_cv_.wait(lock, [this] {
                    return blocked_workers_.load(std::memory_order_acquire) >
                               0 ||
                           !running_.load(std::memory_order_acquire);
                });
            }
        }
        if (!running_.load(std::memory_order_acquire)) break;
        if (blocked_workers_.load(std::memory_order_acquire) == 0) continue;

        const std::int64_t now = steady_now_ns();
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

void ThreadPoolExecutor::set_threads(std::size_t threads,
                                     std::optional<bool> eager) {
    if (threads == 0) threads = hardware_concurrency();
    if (threads == 0) threads = 2;
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    if (threads > 2) threads = 2;
#endif
    std::lock_guard<std::mutex> resize(resize_mutex_);
    const std::size_t old = num_threads_.load(std::memory_order_acquire);
    const bool was_eager = eager_.load(std::memory_order_acquire);
    const bool now_eager = eager.value_or(was_eager);
    eager_.store(now_eager, std::memory_order_release);
    // An elastic pool keeps its floor. One that turns elastic starts at one.
    const std::size_t old_floor = min_workers_.load(std::memory_order_acquire);
    const std::size_t floor =
        now_eager ? threads
                  : std::min(was_eager ? std::size_t{1} : old_floor, threads);
    // The floor first when shrinking and the cap first when growing, so the
    // monitor never sees a floor above the cap.
    if (threads < old) min_workers_.store(floor, std::memory_order_release);
    num_threads_.store(threads, std::memory_order_release);
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
        monitor_thread_ = std::thread(&ThreadPoolExecutor::monitor_loop, this);
    }
    while (live_workers_.load(std::memory_order_acquire) < floor &&
           spawn_worker(floor, /*best_effort=*/false)) {
    }
    DFTRACER_UTILS_LOG_DEBUG("Executor threads %zu -> %zu (floor %zu)", old,
                             threads, floor);
}

void ThreadPoolExecutor::grow_io_threads(std::size_t threads) {
    std::lock_guard<std::mutex> resize(resize_mutex_);
    if (threads <= io_pool_size_.load(std::memory_order_acquire)) return;
    if (io_backend_ && io_backend_->grow_threads(threads))
        io_pool_size_.store(threads, std::memory_order_release);
}

void ThreadPoolExecutor::set_deadlock_timeout(std::chrono::seconds timeout) {
    deadlock_timeout_s_.store(timeout.count(), std::memory_order_relaxed);
}

void ThreadPoolExecutor::monitor_loop() {
    using clock = std::chrono::steady_clock;
    const auto keepalive = keepalive_;
    auto last_busy = clock::now();

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

        if (run_queue_.size_approx() > 0 ||
            idle_workers_.load(std::memory_order_acquire) <
                live_workers_.load(std::memory_order_acquire)) {
            last_busy = clock::now();
        }
        // Grow: spawn until the backlog is covered or we hit the cap - all at
        // once, so a burst reaches full parallelism in one wake.
        while (run_queue_.size_approx() > 0 &&
               live_workers_.load(std::memory_order_acquire) <
                   num_threads_.load(std::memory_order_acquire)) {
            add_worker();
        }
        // Shrink after a keep-alive of sustained idle, by half the workers past
        // the floor each time, so a brief lull costs nothing and a pool of any
        // size reaches its floor in a few keep-alives.
        const std::size_t live = live_workers_.load(std::memory_order_acquire);
        const std::size_t floor = min_workers_.load(std::memory_order_acquire);
        if (live > floor && clock::now() - last_busy >= keepalive) {
            for (std::size_t n = std::max<std::size_t>((live - floor) / 2, 1);
                 n > 0; --n)
                retire_worker();
            last_busy = clock::now();
        }
        lock.lock();
    }
}

void ThreadPoolExecutor::worker_thread(WorkerContext* context) {
    DFTRACER_UTILS_LOG_DEBUG("Worker %zu started", context->worker_id);

    set_current_worker_context(context);
    Executor::set_current(this);
    coro::reset_timeslice();

    // Grows while idle so the pool is not polling, resets on work found.
    std::chrono::milliseconds idle_park = IDLE_PARK_MIN;

    memory::Pool& general = memory::Manager::get().general();

    // Fixed for the process lifetime; read once to keep the resume loop cheap.
    const bool monitor = utilities::monitoring_enabled();
    if (monitor) {
        utilities::monitor_set_worker(static_cast<int>(context->worker_id));
    }

    while (running_ && !context->retire.load(std::memory_order_acquire)) {
        // A cap that shrank leaves the count below zero, so a holder gives its
        // permit back before its next task.
        if (context->holds_permit &&
            permits_.load(std::memory_order_relaxed) < 0) {
            release_permit(context);
        }
        // Hold a permit to run; a surplus thread (live > cap) parks for a slot.
        if (!context->holds_permit) {
            if (!acquire_permit(context)) break;
            context->holds_permit = true;
        }

        RunQueueEntry pending_entry;

        const std::uint64_t observed_signal =
            work_signal_.load(std::memory_order_acquire);

        // Run queue: coroutine handles from enqueue() and
        // schedule_coroutine_resumption().
        // Busy before the dequeue, so quiesce() never sees an empty queue
        // and an idle worker while this worker holds an entry.
        context->is_idle.store(false, std::memory_order_seq_cst);
        if (run_queue_.try_dequeue(pending_entry)) {
            idle_park = IDLE_PARK_MIN;
            coro::reset_timeslice();
            context->resumes.store(
                context->resumes.load(std::memory_order_relaxed) + 1,
                std::memory_order_relaxed);
            std::coroutine_handle<> pending_resume = pending_entry.handle;
            if (pending_resume && !pending_resume.done()) {
                // Id comes from the entry, not the promise: foreign promise
                // types have no task_id field.
                TaskIndex tid = pending_entry.task_id;
                if (tid >= 0) {
                    std::unique_lock<std::shared_mutex> lock(registry_mutex_);
                    auto it = task_registry_.find(tid);
                    if (it != task_registry_.end() &&
                        it->second.state == TaskInfo::Queued) {
                        it->second.state = TaskInfo::Running;
                        it->second.started_at =
                            std::chrono::steady_clock::now();
                        it->second.worker_id = context->worker_id;
                        it->second.location = TaskInfo::Executing;
                    }
                }
                DFTRACER_UTILS_TSAN_ACQUIRE(pending_resume.address());
                if (monitor) {
                    utilities::monitor_resume_begin(pending_entry.monitor_id);
                }
                {
                    // Our coroutines enter their own pool. Anything else
                    // charges the general pool.
                    memory::Scope scope{general, nullptr};
                    pending_resume.resume();
                }
                if (monitor) {
                    utilities::monitor_resume_end(pending_resume.address(),
                                                  pending_resume.done());
                }
            }
            // A batch held until this worker idles would wait behind a later
            // slice that blocks, so ops submitted in this slice start now.
            if (io_backend_) io_backend_->flush();
            // Destroy coroutine frames that FinalAwaiter deferred to this
            // thread.  Safe: resume() has fully returned, so the frame
            // is suspended at final_suspend and no code references it.
            drain_thread_local_destroys();
            drain_destroy_queue();
        }
        // No work available -- sleep until signaled.
        else {
            context->is_idle.store(true, std::memory_order_relaxed);
            // Flush any batched I/O operations before sleeping.
            if (io_backend_) {
                io_backend_->flush();
            }
            // Opportunistic I/O polling before sleeping
            if (io_backend_) {
                auto reaped = io_backend_->poll(0);
                if (reaped > 0) {
                    drain_destroy_queue();
                    continue;
                }
            }
            drain_destroy_queue();
            if (!running_.load(std::memory_order_acquire) ||
                context->retire.load(std::memory_order_acquire)) {
                break;
            }
            // Give up the permit for a waiting/surplus thread, then park.
            release_permit(context);
            idle_workers_.fetch_add(1, std::memory_order_acq_rel);
            // One idle worker at a time backs off from the short park. The rest
            // re-check once after IDLE_PARK_MIN to cover a missed wakeup, then
            // park long.
            const bool poller =
                !idle_poller_.exchange(true, std::memory_order_acq_rel);
            bool idle_long = false;
            {
                std::unique_lock<std::mutex> lock(idle_mutex_);
                if (work_signal_.load(std::memory_order_acquire) !=
                        observed_signal ||
                    idle_cv_.wait_for(lock, idle_park) !=
                        std::cv_status::timeout) {
                    idle_park = IDLE_PARK_MIN;
                } else {
                    idle_long = idle_park == IDLE_PARK_MAX;
                    if (!poller)
                        idle_park = IDLE_PARK_MAX;
                    else if (idle_park < IDLE_PARK_MAX)
                        idle_park *= 2;
                }
            }
            // Only this thread may purge its heap, so an idle worker gives its
            // freed pages back itself.
            if (idle_long) memory::trim_thread();
            if (poller) idle_poller_.store(false, std::memory_order_release);
            idle_workers_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    // Release the permit if we broke out of the loop still holding it.
    release_permit(context);

    // Final drain: destroy any frames deferred during the last resume().
    drain_thread_local_destroys();

    Executor::set_current(nullptr);
    set_current_worker_context(nullptr);

    DFTRACER_UTILS_LOG_DEBUG("Worker %zu terminated", context->worker_id);
}

void ThreadPoolExecutor::drive_until(const std::function<bool()>& done) {
    memory::Pool& general = memory::Manager::get().general();
    // The timeout re-checks the queue for the enqueue that worker_thread's
    // park comment describes.
    std::chrono::milliseconds park = IDLE_PARK_MIN;

    while (!done()) {
        const std::uint64_t observed_signal =
            work_signal_.load(std::memory_order_acquire);

        RunQueueEntry entry;
        if (run_queue_.try_dequeue(entry)) {
            coro::reset_timeslice();
            if (entry.handle && !entry.handle.done()) {
                DFTRACER_UTILS_TSAN_ACQUIRE(entry.handle.address());
                memory::Scope scope{general, nullptr};
                entry.handle.resume();
            }
            drain_thread_local_destroys();
            drain_destroy_queue();
            park = IDLE_PARK_MIN;
            continue;
        }

        drain_destroy_queue();
        if (done()) break;
        if (!running_.load(std::memory_order_acquire)) break;

        if (io_backend_) {
            io_backend_->flush();
            if (io_backend_->poll(0) > 0) {
                park = IDLE_PARK_MIN;
                continue;
            }
        }
        struct Wait {
            const std::function<bool()>& done;
            const ThreadPoolExecutor& self;
            std::uint64_t observed_signal;
        } wait{done, *this, observed_signal};
        const bool woke = coro::park_driver(
            [](void* arg) {
                const auto& w = *static_cast<const Wait*>(arg);
                return w.done() ||
                       !w.self.running_.load(std::memory_order_acquire) ||
                       w.self.work_signal_.load(std::memory_order_acquire) !=
                           w.observed_signal;
            },
            &wait, park);
        park = woke ? IDLE_PARK_MIN : std::min(park * 2, IDLE_PARK_MAX);
    }
    drain_thread_local_destroys();
}

void ThreadPoolExecutor::schedule_coroutine_resumption(
    std::coroutine_handle<> handle) {
    // Delegate to enqueue() -- unified path for all coroutine handles.
    enqueue(handle);
}

void ThreadPoolExecutor::signal_global_work() {
    work_signal_.fetch_add(1, std::memory_order_acq_rel);
    wake_one_worker();
    const std::size_t cap = num_threads_.load(std::memory_order_relaxed);
    if (min_workers_.load(std::memory_order_relaxed) >= cap ||
        live_workers_.load(std::memory_order_relaxed) >= cap) {
        return;
    }
    // A woken worker counts as idle until it runs, so a burst sent before it
    // runs never sees zero idle workers; compare the backlog with them too.
    const std::size_t idle = idle_workers_.load(std::memory_order_acquire);
    if (idle == 0 || run_queue_.size_approx() > idle) {
        {
            std::lock_guard<std::mutex> lock(coord_mutex_);
            grow_pending_ = true;
        }
        coord_cv_.notify_one();
    }
}

void ThreadPoolExecutor::wake_one_worker() {
    work_signal_.notify_one();
    idle_cv_.notify_one();
    coro::wake_parked_drivers();
}

void ThreadPoolExecutor::wake_all_workers() {
    work_signal_.fetch_add(1, std::memory_order_release);
    work_signal_.notify_all();
    idle_cv_.notify_all();
    coro::wake_parked_drivers();
    // Wake workers parked in acquire_permit so they re-check running_/retire.
    permits_.notify_all();
}

// Helper function for when_all.h (avoids circular dependency)

void ThreadPoolExecutor::enqueue(std::coroutine_handle<> handle,
                                 TaskIndex task_id) {
    if (!handle || handle.done()) {
        return;  // Invalid or already completed
    }

    long long monitor_id = -1;
    if (utilities::monitoring_enabled()) {
        // task_id >= 0 marks a tracked submitted task; otherwise it is spawn
        // fan-out (or a re-enqueue of an already-seen coroutine).
        monitor_id = utilities::monitor_enqueue(
            handle.address(), task_id >= 0 ? utilities::CoroKind::Task
                                           : utilities::CoroKind::Spawn);
    }
    const bool foreign = Executor::current() != this;
    if (foreign) foreign_enqueues_.fetch_add(1, std::memory_order_acq_rel);
    DFTRACER_UTILS_TSAN_RELEASE(handle.address());
    run_queue_.enqueue(RunQueueEntry{handle, task_id, monitor_id});
    signal_global_work();
    if (foreign) foreign_enqueues_.fetch_sub(1, std::memory_order_release);
}

bool ThreadPoolExecutor::is_responsive() const {
    if (!running_.load()) {
        return false;
    }

    // Deadlocked means work is pending and no worker has resumed anything for
    // deadlock_timeout_. A task may run for hours while its coroutines resume.
    std::uint64_t resumes = 0;
    bool busy = run_queue_.size_approx() > 0;
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
            num_threads_.load(std::memory_order_relaxed),
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(stalled)
                    .count()));
        return false;
    }
    return true;
}

ExecutorProgress ThreadPoolExecutor::get_progress() const {
    std::shared_lock<std::shared_mutex> lock(registry_mutex_);
    ExecutorProgress progress;

    // Overall stats
    progress.total_tasks_submitted = total_tasks_submitted_.load();
    progress.tasks_completed = tasks_completed_.load();

    // Count task states
    progress.tasks_queued = 0;
    progress.tasks_running = 0;
    progress.tasks_failed = 0;

    for (const auto& [task_id, info] : task_registry_) {
        switch (info.state) {
            case TaskInfo::Queued:
                progress.tasks_queued++;
                break;
            case TaskInfo::Running:
            case TaskInfo::Waiting:
                progress.tasks_running++;
                break;
            case TaskInfo::Completed:
                // Already counted
                break;
            case TaskInfo::Failed:
                progress.tasks_failed++;
                break;
        }

        if (info.state == TaskInfo::Failed && !info.error_message.empty()) {
            progress.recent_errors.push_back({task_id, info.error_message});
        }
    }

    // Build task trees (find root tasks)
    std::unordered_set<TaskIndex> processed;
    for (const auto& [task_id, info] : task_registry_) {
        if (info.parent_task_id == -1) {  // Root task
            auto task_progress = build_task_progress_tree(task_id, processed);
            progress.root_tasks.push_back(task_progress);
        }
    }

    // Worker states (dynamic set; lock against add/retire mutation)
    {
        std::lock_guard<std::mutex> workers_lock(workers_mutex_);
        for (const auto& worker : workers_) {
            ExecutorProgress::WorkerStatus status;
            status.worker_id = worker->worker_id;
            status.is_idle = worker->is_idle.load();

            TaskIndex current_id = worker->current_task_id.load();
            if (current_id != -1) {
                status.current_task_id = current_id;
                std::lock_guard<std::mutex> name_lock(worker->task_name_mutex);
                status.current_task_name = worker->current_task_name;
            }

            status.local_queue_depth = 0;

            progress.workers.push_back(status);
            // No per-worker local queues anymore; report 0.
            progress.worker_queue_depths.push_back(0);
        }
    }

    return progress;
}

TaskProgress ThreadPoolExecutor::build_task_progress_tree(
    TaskIndex task_id, std::unordered_set<TaskIndex>& processed) const {
    TaskProgress progress;

    if (processed.count(task_id)) {
        // Avoid cycles
        progress.task_id = task_id;
        progress.name = "[Cycle Detected]";
        return progress;
    }
    processed.insert(task_id);

    auto it = task_registry_.find(task_id);
    if (it == task_registry_.end()) {
        progress.task_id = task_id;
        progress.name = "[Not Found]";
        return progress;
    }

    const TaskInfo& info = it->second;
    progress.task_id = task_id;
    progress.name = info.name;

    // State
    switch (info.state) {
        case TaskInfo::Queued:
            progress.state = "queued";
            break;
        case TaskInfo::Running:
            progress.state = "running";
            break;
        case TaskInfo::Waiting:
            progress.state = "waiting";
            break;
        case TaskInfo::Completed:
            progress.state = "completed";
            break;
        case TaskInfo::Failed:
            progress.state = "failed";
            break;
    }

    // Timing
    auto now = std::chrono::steady_clock::now();
    if (info.state == TaskInfo::Queued) {
        progress.queued_duration_ms =
            std::chrono::duration<double, std::milli>(now - info.queued_at)
                .count();
        progress.execution_duration_ms = 0;
    } else if (info.state == TaskInfo::Running ||
               info.state == TaskInfo::Waiting) {
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

    // Progress
    progress.total_subtasks = info.child_task_ids.size();
    progress.completed_subtasks = info.completed_children.load();
    if (progress.total_subtasks > 0) {
        progress.progress_percentage =
            (100.0 * static_cast<double>(progress.completed_subtasks)) /
            static_cast<double>(progress.total_subtasks);
    } else {
        progress.progress_percentage =
            (info.state == TaskInfo::Completed) ? 100.0 : 0.0;
    }

    // Location
    switch (info.location) {
        case TaskInfo::SharedQueue:
            progress.location = "shared_queue";
            break;
        case TaskInfo::LocalQueue:
            progress.location =
                "worker_" + std::to_string(info.worker_id) + "_local";
            break;
        case TaskInfo::Executing:
            progress.location =
                "executing_on_worker_" + std::to_string(info.worker_id);
            break;
        case TaskInfo::Done:
            progress.location = "done";
            break;
    }

    // Build children recursively
    for (TaskIndex child_id : info.child_task_ids) {
        progress.children.push_back(
            build_task_progress_tree(child_id, processed));
    }

    return progress;
}

// ============================================================================
// Phase 3: Coro-based task execution
// ============================================================================

coro::Coro ThreadPoolExecutor::run_task(std::shared_ptr<Task> task,
                                        std::shared_ptr<std::any> input,
                                        TaskOwner* owner) {
    // Get worker context from TLS (set by worker_thread at start).
    auto* context = static_cast<WorkerContext*>(get_current_worker_context());

    if (!task) {
        co_return;
    }
    // Tells the owner the task ended, also when the queued frame is destroyed
    // unrun at shutdown.
    struct FinishTask {
        TaskOwner* owner;
        std::shared_ptr<Task> task;
        bool ran = false;
        ~FinishTask() {
            if (!ran)
                task->set_exception(std::make_exception_ptr(
                    PipelineError(PipelineError::Interrupted,
                                  "the pipeline of the task stopped")));
            owner->finish_task(task);
        }
    } finish{owner, task};
    if (!context || owner->is_stopping()) {
        ++tasks_completed_;
        co_return;
    }

    // Update worker context
    context->current_task_id = task->get_id();
    {
        std::lock_guard<std::mutex> lock(context->task_name_mutex);
        context->current_task_name = task->get_name();
    }
    context->last_activity = std::chrono::steady_clock::now();

    // Update task registry to RUNNING
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        auto it = task_registry_.find(task->get_id());
        if (it != task_registry_.end()) {
            it->second.state = TaskInfo::Running;
            it->second.started_at = std::chrono::steady_clock::now();
            it->second.worker_id = context->worker_id;
            it->second.location = TaskInfo::Executing;
        }
    }

    task->result().mark_running();

    {
        std::exception_ptr task_error;

        DFTRACER_UTILS_LOG_DEBUG("Worker %zu executing task ID %ld ('%s')",
                                 context->worker_id, task->get_id(),
                                 task->get_name());

        // The body never throws and records task_error.
        co_await run_coro_scope(
            this, [&, this](CoroScope& scope) -> coro::CoroTask<void> {
                try {
                    auto coro_task = task->execute(scope, *input);
                    // Propagate executor to the CoroTask's PromiseBase so that
                    // nested awaitables (when_any, channel, etc.) can schedule
                    // resumptions.  run_task() is a Coro (CoroPromise, not
                    // PromiseBase), so the normal PromiseBase propagation in
                    // CoroTask::await_suspend doesn't fire.
                    coro_task.handle().promise().set_executor(this);
                    std::any result = co_await std::move(coro_task);

                    task->set_result(std::move(result));
                } catch (...) {
                    task_error = std::current_exception();
                }
            });

        // After the awaits we may have resumed on a different worker; the
        // starting `context` can be freed (retired), so touch the live resumer.
        auto* finishing =
            static_cast<WorkerContext*>(get_current_worker_context());

        if (!task_error) {
            DFTRACER_UTILS_LOG_DEBUG(
                "Task ID %ld ('%s') completed successfully", task->get_id(),
                task->get_name());

            ++tasks_completed_;
            if (finishing) finishing->tasks_executed++;

            // Update task registry to COMPLETED
            {
                std::unique_lock<std::shared_mutex> lock(registry_mutex_);
                auto it = task_registry_.find(task->get_id());
                if (it != task_registry_.end()) {
                    it->second.state = TaskInfo::Completed;
                    it->second.completed_at = std::chrono::steady_clock::now();
                    it->second.location = TaskInfo::Done;

                    // Update parent's completed children count
                    if (it->second.parent_task_id != -1) {
                        auto parent_it =
                            task_registry_.find(it->second.parent_task_id);
                        if (parent_it != task_registry_.end()) {
                            parent_it->second.completed_children++;
                        }
                    }
                }
            }

            // Notify scheduler
            finish.ran = true;

        } else {
            try {
                std::rethrow_exception(task_error);
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_ERROR("Task ID %ld ('%s') failed: %s",
                                         task->get_id(), task->get_name(),
                                         e.what());

                task->set_exception(task_error);

                ++tasks_completed_;
                if (finishing) finishing->tasks_executed++;

                {
                    std::unique_lock<std::shared_mutex> lock(registry_mutex_);
                    auto it = task_registry_.find(task->get_id());
                    if (it != task_registry_.end()) {
                        it->second.state = TaskInfo::Failed;
                        it->second.completed_at =
                            std::chrono::steady_clock::now();
                        it->second.error_message = e.what();
                        it->second.location = TaskInfo::Done;
                    }
                }

                finish.ran = true;

            } catch (...) {
                DFTRACER_UTILS_LOG_ERROR(
                    "Task ID %ld ('%s') failed with unknown exception",
                    task->get_id(), task->get_name());

                task->set_exception(task_error);

                ++tasks_completed_;
                if (finishing) finishing->tasks_executed++;

                {
                    std::unique_lock<std::shared_mutex> lock(registry_mutex_);
                    auto it = task_registry_.find(task->get_id());
                    if (it != task_registry_.end()) {
                        it->second.state = TaskInfo::Failed;
                        it->second.completed_at =
                            std::chrono::steady_clock::now();
                        it->second.error_message = "Unknown exception";
                        it->second.location = TaskInfo::Done;
                    }
                }

                finish.ran = true;
            }
        }
    }

    // Clear current-task info only on the worker still showing this task: after
    // migration the starting worker's context may be freed or already onto its
    // next task.
    if (auto* cur = static_cast<WorkerContext*>(get_current_worker_context());
        cur && cur->current_task_id.load(std::memory_order_relaxed) ==
                   task->get_id()) {
        cur->current_task_id = -1;
        std::lock_guard<std::mutex> lock(cur->task_name_mutex);
        cur->current_task_name.clear();
    }

    co_return;
}

void ThreadPoolExecutor::submit_task(std::shared_ptr<Task> task,
                                     std::shared_ptr<std::any> input,
                                     TaskOwner* owner,
                                     TaskIndex parent_task_id) {
    // Register in task registry
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);

        auto [it, inserted] = task_registry_.emplace(
            std::piecewise_construct, std::forward_as_tuple(task->get_id()),
            std::forward_as_tuple());

        if (inserted) {
            it->second.task_id = task->get_id();
            it->second.parent_task_id = parent_task_id;
            it->second.name = task->get_name();
            it->second.state = TaskInfo::Queued;
            it->second.queued_at = std::chrono::steady_clock::now();
            it->second.location = TaskInfo::SharedQueue;
            it->second.worker_id = static_cast<std::size_t>(-1);

            if (parent_task_id != -1) {
                auto parent_it = task_registry_.find(parent_task_id);
                if (parent_it != task_registry_.end()) {
                    parent_it->second.child_task_ids.push_back(task->get_id());
                }
            }
        }
    }

    ++total_tasks_submitted_;

    // Create Coro, set executor on promise, enqueue released handle
    auto coro = run_task(std::move(task), std::move(input), owner);
    coro.handle().promise().executor = this;
    enqueue(coro.release());
}

TaskIndex ThreadPoolExecutor::enqueue_tracked(
    coro::Coro coro, std::string name,
    std::shared_ptr<std::atomic<TaskIndex>> tid_out) {
    TaskIndex id = next_coro_task_id_.fetch_sub(1, std::memory_order_relaxed);
    coro.handle().promise().task_id = id;
    coro.handle().promise().executor = this;

    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        auto [it, inserted] = task_registry_.emplace(std::piecewise_construct,
                                                     std::forward_as_tuple(id),
                                                     std::forward_as_tuple());
        if (inserted) {
            auto now = std::chrono::steady_clock::now();
            it->second.task_id = id;
            it->second.parent_task_id = -1;
            it->second.name = std::move(name);
            it->second.state = TaskInfo::Queued;
            it->second.queued_at = now;
            it->second.location = TaskInfo::SharedQueue;
            it->second.worker_id = static_cast<std::size_t>(-1);
        }
    }
    ++total_tasks_submitted_;

    if (tid_out) {
        tid_out->store(id, std::memory_order_release);
    }

    auto handle = coro.release();
    enqueue(handle, id);
    return id;
}

TaskIndex ThreadPoolExecutor::current_worker_task_id() const {
    auto* ctx = static_cast<WorkerContext*>(get_current_worker_context());
    return ctx ? ctx->current_task_id.load() : -1;
}

void ThreadPoolExecutor::mark_coro_completed(TaskIndex id) {
    bool was_new = false;
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        auto it = task_registry_.find(id);
        if (it != task_registry_.end() &&
            it->second.state != TaskInfo::Completed) {
            auto now = std::chrono::steady_clock::now();
            if (it->second.started_at.time_since_epoch().count() == 0) {
                it->second.started_at = now;
            }
            it->second.state = TaskInfo::Completed;
            it->second.completed_at = now;
            it->second.location = TaskInfo::Done;
            was_new = true;
        }
    }
    if (was_new) ++tasks_completed_;
}

void ThreadPoolExecutor::schedule_destroy(std::coroutine_handle<> handle) {
    if (handle) {
        destroy_queue_.enqueue(handle);
    }
}

void ThreadPoolExecutor::drain_destroy_queue() {
    std::coroutine_handle<> to_destroy;
    while (destroy_queue_.try_dequeue(to_destroy)) {
        if (to_destroy) {
            to_destroy.destroy();
        }
    }
}

}  // namespace dftracer::utils
