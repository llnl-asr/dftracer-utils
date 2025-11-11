#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_FUTURE_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_FUTURE_H

#include <dftracer/utils/core/common/typedefs.h>

#include <any>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <thread>

namespace dftracer::utils {

// Forward declarations
class Task;
class Scheduler;

/**
 * TaskFuture<T> - Awaitable future type for task results
 *
 * Usage:
 * @code
 * // In coroutine:
 * auto future = ctx.spawn([]() -> Task<int> {
 *     co_return compute();
 * });
 * int result = co_await future;
 *
 * // In non-coroutine:
 * int result = future.get();  // Blocks until complete
 * @endcode
 */
template <typename T = void>
class TaskFuture {
   private:
    std::shared_ptr<Task> task_;
    TaskIndex task_id_;
    Scheduler* scheduler_;

    std::shared_ptr<std::atomic<bool>> completed_;
    std::shared_ptr<std::atomic<bool>> cancellation_token_;

    // Allow conversion constructors to access private members
    template <typename U>
    friend class TaskFuture;

   public:
    // Type aliases for combinators
    using value_type = T;
    using result_type = T;

    /**
     * Constructor
     * @param task The task this future represents
     * @param task_id Task identifier
     * @param scheduler Scheduler for completion callback registration
     * @param cancellation_token Optional cancellation token for cooperative
     * cancellation
     */
    TaskFuture(std::shared_ptr<Task> task, TaskIndex task_id,
               Scheduler* scheduler,
               std::shared_ptr<std::atomic<bool>> cancellation_token = nullptr)
        : task_(task),
          task_id_(task_id),
          scheduler_(scheduler),
          completed_(std::make_shared<std::atomic<bool>>(false)),
          cancellation_token_(cancellation_token) {}

    /**
     * Default constructor (invalid future)
     */
    TaskFuture()
        : task_(nullptr),
          task_id_(0),
          scheduler_(nullptr),
          completed_(std::make_shared<std::atomic<bool>>(false)),
          cancellation_token_(nullptr) {}

    /**
     * Implicit conversion constructor from TaskFuture<std::any>
     *
     * This allows TaskFuture<std::any> to be implicitly converted to
     * TaskFuture<T> while sharing the same internal state (no duplicate futures
     * created). The shared_ptr members are copied, meaning both futures share
     * the same completed_ flag and cancellation_token_.
     *
     * @param any_future The TaskFuture<std::any> to convert from
     */
    template <typename U = T>
    TaskFuture(const TaskFuture<std::any>& any_future,
               typename std::enable_if<!std::is_same<U, std::any>::value,
                                       int>::type = 0)
        : task_(any_future.get_task()),
          task_id_(any_future.get_task_id()),
          scheduler_(any_future.get_scheduler()),
          completed_(any_future.get_completed_flag()),
          cancellation_token_(any_future.get_cancellation_token()) {}

    /**
     * Implicit conversion constructor from any TaskFuture<U> to
     * TaskFuture<std::any>
     *
     * Allows TaskFuture<U> to be implicitly converted to TaskFuture<std::any>
     * while sharing the same internal state.
     * Only enabled when T is std::any and U is not std::any.
     */
    template <typename U>
    TaskFuture(const TaskFuture<U>& typed_future,
               typename std::enable_if<std::is_same<T, std::any>::value &&
                                           !std::is_same<U, std::any>::value,
                                       int>::type = 0)
        : task_(typed_future.get_task()),
          task_id_(typed_future.get_task_id()),
          scheduler_(typed_future.get_scheduler()),
          completed_(typed_future.get_completed_flag()),
          cancellation_token_(typed_future.get_cancellation_token()) {}

    // Copyable (shared ownership semantics)
    TaskFuture(const TaskFuture&) = default;
    TaskFuture& operator=(const TaskFuture&) = default;

    // Movable
    TaskFuture(TaskFuture&&) noexcept = default;
    TaskFuture& operator=(TaskFuture&&) noexcept = default;

    // ========================================================================
    // Awaitable interface (for coroutines)
    // ========================================================================

    /**
     * Check if task already completed (optimization to avoid suspension)
     */
    bool await_ready() const noexcept {
        return completed_->load(std::memory_order_acquire);
    }

    /**
     * Suspend current coroutine, register callback to resume when task
     * completes
     *
     * @param awaiting The coroutine handle that is awaiting this future
     * @return noop_coroutine for symmetric transfer (tells executor not to
     * resume)
     */
    template <typename Promise>
    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<Promise> awaiting);

    /**
     * Called when coroutine resumes - return result or throw exception
     *
     * @return The result value (for non-void T)
     * @throws Exception if task threw
     */
    T await_resume();

    // ========================================================================
    // Internal methods (called by executor on task completion)
    // ========================================================================

    /**
     * Mark as completed (called by executor thread)
     */
    void mark_completed() {
        completed_->store(true, std::memory_order_release);
    }

    // ========================================================================
    // Non-coroutine interface (blocking)
    // ========================================================================

    /**
     * Blocking get - waits for task completion
     * Waits on the underlying task's future instead of the completed flag
     *
     * @return The result value
     * @throws Exception if task threw
     */
    T get();

    /**
     * Blocking wait - waits for task completion without retrieving result
     */
    void wait() const {
        while (!completed_->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    /**
     * Wait with timeout
     *
     * @param timeout_duration Maximum time to wait
     * @return future_status indicating if ready, timeout, or deferred
     */
    template <class Rep, class Period>
    std::future_status wait_for(
        const std::chrono::duration<Rep, Period>& timeout_duration) const {
        auto start = std::chrono::steady_clock::now();
        while (!completed_->load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() - start >= timeout_duration) {
                return std::future_status::timeout;
            }
            std::this_thread::yield();
        }
        return std::future_status::ready;
    }

    /**
     * Wait until specific time point
     *
     * @param timeout_time Time point to wait until
     * @return future_status indicating if ready, timeout, or deferred
     */
    template <class Clock, class Duration>
    std::future_status wait_until(
        const std::chrono::time_point<Clock, Duration>& timeout_time) const {
        while (!completed_->load(std::memory_order_acquire)) {
            if (Clock::now() >= timeout_time) {
                return std::future_status::timeout;
            }
            std::this_thread::yield();
        }
        return std::future_status::ready;
    }

    /**
     * Check if task completed
     * Uses acquire memory ordering
     */
    bool is_ready() const noexcept {
        return completed_->load(std::memory_order_acquire);
    }

    /**
     * Check if future is valid
     */
    bool valid() const noexcept { return task_ != nullptr; }

    /**
     * Get underlying task
     */
    std::shared_ptr<Task> get_task() const noexcept { return task_; }

    /**
     * Get task ID
     */
    TaskIndex get_task_id() const noexcept { return task_id_; }

    /**
     * Request cancellation of this task
     * This is cooperative - task must check ctx.is_cancellation_requested()
     */
    void request_cancellation() {
        if (cancellation_token_) {
            cancellation_token_->store(true, std::memory_order_release);
        }
    }

    /**
     * Check if cancellation has been requested
     */
    bool is_cancellation_requested() const {
        return cancellation_token_ &&
               cancellation_token_->load(std::memory_order_acquire);
    }

    /**
     * Get the cancellation token
     */
    std::shared_ptr<std::atomic<bool>> get_cancellation_token() const {
        return cancellation_token_;
    }

    /**
     * Get the scheduler pointer (for conversion constructor)
     */
    Scheduler* get_scheduler() const noexcept { return scheduler_; }

    /**
     * Get the completed flag (for conversion constructor)
     */
    std::shared_ptr<std::atomic<bool>> get_completed_flag() const {
        return completed_;
    }
};

/**
 * Specialization for void return type
 * Simpler implementation without result storage
 */
template <>
class TaskFuture<void> {
   private:
    std::shared_ptr<Task> task_;
    TaskIndex task_id_;
    Scheduler* scheduler_;

    std::shared_ptr<std::atomic<bool>> completed_;
    std::shared_ptr<std::atomic<bool>> cancellation_token_;

    // Allow conversion constructors to access private members
    template <typename U>
    friend class TaskFuture;

   public:
    using value_type = void;
    using result_type = void;

    TaskFuture(std::shared_ptr<Task> task, TaskIndex task_id,
               Scheduler* scheduler,
               std::shared_ptr<std::atomic<bool>> cancellation_token = nullptr)
        : task_(task),
          task_id_(task_id),
          scheduler_(scheduler),
          completed_(std::make_shared<std::atomic<bool>>(false)),
          cancellation_token_(cancellation_token) {}

    TaskFuture()
        : task_(nullptr),
          task_id_(0),
          scheduler_(nullptr),
          completed_(std::make_shared<std::atomic<bool>>(false)),
          cancellation_token_(nullptr) {}

    /**
     * Implicit conversion constructor from TaskFuture<std::any>
     *
     * Allows TaskFuture<std::any> to be implicitly converted to
     * TaskFuture<void> while sharing the same internal state.
     *
     * @param any_future The TaskFuture<std::any> to convert from
     */
    TaskFuture(const TaskFuture<std::any>& any_future)
        : task_(any_future.get_task()),
          task_id_(any_future.get_task_id()),
          scheduler_(any_future.get_scheduler()),
          completed_(any_future.get_completed_flag()),
          cancellation_token_(any_future.get_cancellation_token()) {}

    /**
     * Implicit conversion constructor from any TaskFuture<T>
     *
     * Allows TaskFuture<T> to be implicitly converted to TaskFuture<void>
     * while sharing the same internal state (for TaskScope storage).
     * Excludes void and std::any (handled by other constructors).
     *
     * @param typed_future The TaskFuture<T> to convert from
     */
    template <typename T>
    TaskFuture(const TaskFuture<T>& typed_future,
               typename std::enable_if<!std::is_same<T, void>::value &&
                                           !std::is_same<T, std::any>::value,
                                       int>::type = 0)
        : task_(typed_future.get_task()),
          task_id_(typed_future.get_task_id()),
          scheduler_(typed_future.get_scheduler()),
          completed_(typed_future.get_completed_flag()),
          cancellation_token_(typed_future.get_cancellation_token()) {}

    TaskFuture(const TaskFuture&) = default;
    TaskFuture& operator=(const TaskFuture&) = default;
    TaskFuture(TaskFuture&&) noexcept = default;
    TaskFuture& operator=(TaskFuture&&) noexcept = default;

    bool await_ready() const noexcept {
        return completed_->load(std::memory_order_acquire);
    }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<Promise> awaiting);

    void await_resume();

    void mark_completed() {
        completed_->store(true, std::memory_order_release);
    }

    void get();

    void wait() const {
        while (!completed_->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    template <class Rep, class Period>
    std::future_status wait_for(
        const std::chrono::duration<Rep, Period>& timeout_duration) const {
        auto start = std::chrono::steady_clock::now();
        while (!completed_->load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() - start >= timeout_duration) {
                return std::future_status::timeout;
            }
            std::this_thread::yield();
        }
        return std::future_status::ready;
    }

    template <class Clock, class Duration>
    std::future_status wait_until(
        const std::chrono::time_point<Clock, Duration>& timeout_time) const {
        while (!completed_->load(std::memory_order_acquire)) {
            if (Clock::now() >= timeout_time) {
                return std::future_status::timeout;
            }
            std::this_thread::yield();
        }
        return std::future_status::ready;
    }

    bool is_ready() const noexcept {
        return completed_->load(std::memory_order_acquire);
    }

    bool valid() const noexcept { return task_ != nullptr; }

    std::shared_ptr<Task> get_task() const noexcept { return task_; }

    TaskIndex get_task_id() const noexcept { return task_id_; }

    void request_cancellation() {
        if (cancellation_token_) {
            cancellation_token_->store(true, std::memory_order_release);
        }
    }

    bool is_cancellation_requested() const {
        return cancellation_token_ &&
               cancellation_token_->load(std::memory_order_acquire);
    }

    std::shared_ptr<std::atomic<bool>> get_cancellation_token() const {
        return cancellation_token_;
    }

    /**
     * Get the scheduler pointer (for conversion constructor)
     */
    Scheduler* get_scheduler() const noexcept { return scheduler_; }

    /**
     * Get the completed flag (for conversion constructor)
     */
    std::shared_ptr<std::atomic<bool>> get_completed_flag() const {
        return completed_;
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_FUTURE_H
