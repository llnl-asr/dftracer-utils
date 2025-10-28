#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_FUTURE_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_FUTURE_H

#include <any>
#include <chrono>
#include <future>
#include <memory>

namespace dftracer::utils {

// Forward declarations
class Executor;

/**
 * TaskFuture - A future returned by task submission that handles work-stealing
 * transparently
 *
 * This is a drop-in replacement for std::shared_future<std::any> that
 * automatically performs work-stealing when get() is called from within a task
 * execution context. This prevents deadlock when tasks submit and wait for
 * child tasks.
 */
class TaskFuture {
   private:
    std::shared_future<std::any> inner_future_;

   public:
    /**
     * Constructor from shared_future
     */
    explicit TaskFuture(std::shared_future<std::any> future)
        : inner_future_(std::move(future)) {}

    /**
     * Default constructor (creates an invalid future)
     */
    TaskFuture() = default;

    /**
     * Get the result
     */
    std::any get() const;

    /**
     * Get the result with type casting
     */
    template <typename T>
    T get() const {
        return std::any_cast<T>(get());
    }

    /**
     * Check if the future is valid
     */
    bool valid() const { return inner_future_.valid(); }

    /**
     * Wait for the future to become ready
     */
    void wait() const;

    /**
     * Wait for the future with a timeout
     */
    template <class Rep, class Period>
    std::future_status wait_for(
        const std::chrono::duration<Rep, Period>& timeout_duration) const {
        return inner_future_.wait_for(timeout_duration);
    }

    /**
     * Wait until a specific time point
     */
    template <class Clock, class Duration>
    std::future_status wait_until(
        const std::chrono::time_point<Clock, Duration>& timeout_time) const {
        return inner_future_.wait_until(timeout_time);
    }

    /**
     * Implicit conversion to std::shared_future for backward compatibility
     */
    operator std::shared_future<std::any>() const { return inner_future_; }

    template <typename T>
    operator std::shared_future<T>() const {
        return std::static_pointer_cast<std::shared_future<T>>(
            std::make_shared<std::shared_future<std::any>>(inner_future_));
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_FUTURE_H
