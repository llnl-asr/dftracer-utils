#ifndef DFTRACER_UTILS_RUNTIME_EXECUTOR_RUN_LOOP_H
#define DFTRACER_UTILS_RUNTIME_EXECUTOR_RUN_LOOP_H

#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/executor/parked.h>
#include <dftracer/utils/runtime/executor/timer_service.h>

#include <condition_variable>
#include <coroutine>
#include <deque>
#include <functional>
#include <mutex>

namespace dftracer::utils {

/**
 * RunLoop - an Executor that runs coroutines on the calling thread.
 *
 * Owns no threads: `drive_until` resumes ready coroutines in turn until the
 * predicate holds. Concurrency without parallelism, which is what a caller
 * that supplied no runtime should get.
 *
 * Reports no I/O backend, so `io::` calls take their blocking-syscall path
 * rather than waiting on completions nothing would poll.
 */
class RunLoop : public Executor {
   public:
    RunLoop() = default;

    RunLoop(const RunLoop&) = delete;
    RunLoop& operator=(const RunLoop&) = delete;

    void enqueue(std::coroutine_handle<> handle,
                 TaskIndex task_id = -1) override;
    void schedule_coroutine_resumption(std::coroutine_handle<> handle) override;
    void schedule_destroy(std::coroutine_handle<> handle) override;
    void mark_coro_completed(TaskIndex id) override;

    bool is_running() const override { return true; }
    std::size_t get_num_threads() const override { return 1; }
    std::size_t get_io_pool_size() const override { return 0; }
    TimerService& get_timer_service() override { return timer_service_; }

    bool runs_remote_wakeups() const noexcept override { return false; }
    bool has_io_backend() const noexcept override { return false; }
    io::IoBackend& io_backend() override;

    /**
     * Resume ready coroutines until `done()`, then run whatever the finished
     * work left runnable until the queue is empty.
     *
     * With the queue empty and `done()` false, waits while a parked waiter
     * (see park()) exists, since another thread can still wake it. Throws
     * when there is none: this loop is the only thing that could resume the
     * awaited work, so waiting longer would hang forever.
     */
    void drive_until(const std::function<bool()>& done) override;

    ParkNode* park(void* waiter, ParkCancelFn cancel) noexcept override {
        return parked_.park(waiter, cancel);
    }
    void unpark(ParkNode* n) noexcept override { parked_.unpark(n); }

   private:
    bool run_one();
    void drain_destroys();
    [[noreturn]] void throw_stalled();

    std::mutex mutex_;
    std::condition_variable ready_cv_;
    ParkRegistry parked_;
    std::deque<std::coroutine_handle<>> ready_;
    std::deque<std::coroutine_handle<>> pending_destroy_;
    TimerService timer_service_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_EXECUTOR_RUN_LOOP_H
