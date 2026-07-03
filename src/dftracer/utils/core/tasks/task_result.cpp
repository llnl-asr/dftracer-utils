#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/tasks/task_result.h>

#include <cassert>

namespace dftracer::utils {

void TaskResult::set_value(std::any value) {
    value_ = std::move(value);
    publish(State::value);
}

void TaskResult::set_exception(std::exception_ptr ex) {
    exception_ = ex;
    publish(State::exception);
}

void TaskResult::set_cancelled() { publish(State::cancelled); }

void TaskResult::mark_running() {
    auto expected = static_cast<std::uint8_t>(State::pending);
    state_.compare_exchange_strong(
        expected, static_cast<std::uint8_t>(State::running),
        std::memory_order_release, std::memory_order_relaxed);
}

void TaskResult::add_reader() {
    pending_readers_.fetch_add(1, std::memory_order_relaxed);
}

void TaskResult::release_reader() {
    int prev = pending_readers_.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) {
        // Last reader released -- free value memory
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = std::any{};
    }
}

void TaskResult::publish(State s) {
    std::vector<std::coroutine_handle<>> to_resume;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.store(static_cast<std::uint8_t>(s), std::memory_order_release);
        to_resume.swap(continuations_);
    }
    cv_.notify_all();
    // Resume coroutine continuations OUTSIDE the lock.
    for (auto h : to_resume) {
        if (h && !h.done()) {
            h.resume();
        }
    }
}

bool TaskResult::wait(std::chrono::milliseconds timeout) const {
    auto ready = [this] {
        auto s = static_cast<State>(state_.load(std::memory_order_acquire));
        return s == State::value || s == State::exception ||
               s == State::cancelled;
    };
    if (ready()) return true;

    std::unique_lock<std::mutex> lock(mutex_);
    if (timeout.count() == 0) {
        cv_.wait(lock, ready);
        return true;
    }
    return cv_.wait_for(lock, timeout, ready);
}

std::any TaskResult::get() const {
    wait();
    std::lock_guard<std::mutex> lock(mutex_);
    auto s = static_cast<State>(state_.load(std::memory_order_acquire));
    if (s == State::exception) {
        std::rethrow_exception(exception_);
    }
    if (s == State::cancelled) {
        throw DFTUtilsException(ErrorCode::PIPELINE, "Task was cancelled");
    }
    return value_;  // returns COPY
}

std::any TaskResult::get_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto s = static_cast<State>(state_.load(std::memory_order_acquire));
    assert(s == State::value || s == State::exception || s == State::cancelled);
    if (s == State::exception) {
        std::rethrow_exception(exception_);
    }
    if (s == State::cancelled) {
        throw DFTUtilsException(ErrorCode::PIPELINE, "Task was cancelled");
    }
    return value_;  // returns COPY
}

std::exception_ptr TaskResult::get_exception() const { return exception_; }

bool TaskResult::is_ready() const {
    auto s = static_cast<State>(state_.load(std::memory_order_acquire));
    return s == State::value || s == State::exception || s == State::cancelled;
}

bool TaskResult::has_exception() const {
    return static_cast<State>(state_.load(std::memory_order_acquire)) ==
           State::exception;
}

bool TaskResult::is_cancelled() const {
    return static_cast<State>(state_.load(std::memory_order_acquire)) ==
           State::cancelled;
}

TaskResult::State TaskResult::state() const {
    return static_cast<State>(state_.load(std::memory_order_acquire));
}

// WhenReadyAwaitable
bool TaskResult::WhenReadyAwaitable::await_ready() const noexcept {
    return result.is_ready();
}

bool TaskResult::WhenReadyAwaitable::await_suspend(std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(result.mutex_);
    // Double-check under lock -- result may have been published
    // between await_ready() and await_suspend().
    if (result.is_ready()) {
        return false;  // Don't suspend, resume immediately
    }
    result.continuations_.push_back(h);
    return true;       // Suspend
}

void TaskResult::WhenReadyAwaitable::await_resume() {
    if (result.has_exception()) {
        std::rethrow_exception(result.exception_);
    }
}

}  // namespace dftracer::utils
