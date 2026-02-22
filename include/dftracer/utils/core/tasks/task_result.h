#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_RESULT_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_RESULT_H

#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <vector>

namespace dftracer::utils {

/// Lightweight one-shot result holder for Task.
/// Supports both blocking wait (tests, foreground) and co_await (runtime).
/// ~48 bytes embedded in Task. No heap allocation for shared state.
class TaskResult {
   public:
    enum class State : std::uint8_t {
        pending,    // Not started
        running,    // Execution in progress
        value,      // Completed with value
        exception,  // Completed with exception
        cancelled,  // Cancelled before completion
    };

    TaskResult() = default;
    ~TaskResult() = default;

    TaskResult(const TaskResult&) = delete;
    TaskResult& operator=(const TaskResult&) = delete;
    TaskResult(TaskResult&&) = delete;
    TaskResult& operator=(TaskResult&&) = delete;

    // === Write API (called ONCE by executor on completion) ===

    void set_value(std::any value);
    void set_exception(std::exception_ptr ex);
    void set_cancelled();
    void mark_running();

    // === Smart Value Release (push-model memory optimization) ===

    /// Register that one more consumer will read the value.
    /// Called by Task::depends_on() -- one per child edge.
    void add_reader();

    /// Signal that one consumer is done with the value.
    /// Called by Scheduler after preparing a child's input.
    /// When last reader releases, value_ is cleared (memory freed).
    /// Terminal tasks (no children) have reader_count_==0 and are
    /// never auto-released -- their value persists for user get().
    void release_reader();

    // === Blocking Read API (tests, scheduler, pipeline) ===

    /// Block until ready. Returns false on timeout.
    /// timeout of 0ms means wait forever.
    bool wait(std::chrono::milliseconds timeout = std::chrono::milliseconds{
                  0}) const;

    /// Block until ready, return copy of value. Throws if
    /// exception/cancelled. Safe to call concurrently with
    /// release_reader() (serialized by mutex).
    std::any get() const;

    /// Return value without blocking. Asserts ready state.
    /// Use only when caller KNOWS task is complete (e.g., scheduler
    /// processing a ready child whose parent is guaranteed complete).
    /// Returns a copy (value may be released after this call).
    std::any get_ready() const;

    /// Return exception without blocking. nullptr if no exception.
    std::exception_ptr get_exception() const;

    // === State queries ===

    bool is_ready() const;
    bool has_exception() const;
    bool is_cancelled() const;
    State state() const;

    // === Coroutine Read API (runtime threads) ===

    /// Awaitable that suspends caller until result is ready.
    /// If already ready, resumes immediately.
    struct WhenReadyAwaitable {
        TaskResult& result;

        bool await_ready() const noexcept;
        bool await_suspend(std::coroutine_handle<> h);
        void await_resume();
    };

    WhenReadyAwaitable when_ready() { return WhenReadyAwaitable{*this}; }

   private:
    void publish(State s);

    std::any value_;
    std::exception_ptr exception_;
    std::atomic<std::uint8_t> state_{static_cast<std::uint8_t>(State::pending)};

    // Reader tracking for smart value release.
    // Starts at 0. Incremented by add_reader() (called from depends_on).
    // Decremented by release_reader() (called by Scheduler after consuming).
    // Value cleared when count goes from 1 -> 0.
    // Terminal tasks (count stays 0) are never auto-released.
    std::atomic<int> pending_readers_{0};

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;

    // Coroutine continuations waiting for completion.
    // Protected by mutex_. Drained on publish().
    std::vector<std::coroutine_handle<>> continuations_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_RESULT_H
