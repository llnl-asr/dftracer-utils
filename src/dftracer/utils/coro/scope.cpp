// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fatal.h>

#include <cstdlib>
#include <exception>
#include <utility>

namespace dftracer::utils::coro {

Scope::~Scope() {
    if (!joined_ && spawned_.load(std::memory_order_acquire)) {
        DFTRACER_UTILS_LOG_ERROR(
            "{}", "Scope destroyed without join() after spawning");
        std::terminate();
    }
}

void Scope::spawn(CoroTask<void> task) {
    if (!admit()) return;
    bind(task.handle().promise());
    detail::Coro child = run_child(std::move(task), this);
    start(child);
}

namespace detail {
bool scope_stopping(const Scope& scope) noexcept {
    return scope.stop_.load(std::memory_order_acquire) ||
           scope.linked_.stop_requested() ||
           (scope.owner_ && scope.owner_->cancel_requested());
}
}  // namespace detail

void Scope::request_stop() noexcept {
    stop_.store(true, std::memory_order_release);
    if (executor_) executor_->wake_cancelled();
}

bool Scope::stop_requested() const noexcept {
    if (detail::scope_stopping(*this)) return true;
    if (executor_ == nullptr) return false;
    auto token = executor_->shutdown_token();
    return token && token->load(std::memory_order_acquire);
}

bool Scope::is_refused() const {
    if (closed_.load(std::memory_order_acquire)) {
        dstd::fatal("coro: a spawn into a closed scope");
    }
    if (executor_ == nullptr) {
        dstd::fatal("coro: a spawn into a scope without a running executor");
    }
    auto token = executor_->shutdown_token();
    return token && token->load(std::memory_order_acquire);
}

bool Scope::admit() {
    if (!is_refused()) return true;
    // Counted as spawned, so a refused spawn still needs the join.
    spawned_.store(true, std::memory_order_release);
    note_child_stopped();
    if (mode_ == ScopeMode::FailFast) request_stop();
    return false;
}

void Scope::fail(std::exception_ptr error) noexcept {
    if (!failed_.exchange(true, std::memory_order_acq_rel)) {
        error_ = std::move(error);
    }
    if (mode_ == ScopeMode::FailFast) request_stop();
}

void Scope::child_sink(void* scope, std::exception_ptr error) noexcept {
    static_cast<Scope*>(scope)->child_failed(std::move(error));
}

void Scope::child_failed(std::exception_ptr error) noexcept {
    fail(std::move(error));
}

void Scope::note_child_stopped() noexcept {
    // The stop that ended this child is not an event, or a plain request_stop
    // would stop the task that holds the scope. A stop from outside, such as
    // at shutdown, is: the siblings get their own cancel, and a stop here
    // would also end the ones that are not parked and would have finished.
    if (stop_.load(std::memory_order_acquire)) return;
    child_stopped_.store(true, std::memory_order_release);
}

void Scope::start(detail::Coro& child) {
    join_.track(child);
    auto& promise = child.handle().promise();
    if (promise.exception_sink == nullptr) {
        promise.exception_sink = &Scope::child_sink;
        promise.sink_owner = this;
    }
    spawned_.store(true, std::memory_order_release);
    child.handle().promise().executor = executor_;
    executor_->post(child.release());
}

void Scope::finish_join() noexcept {
    joined_ = true;
    close();
}

detail::Coro Scope::run_child(CoroTask<void> task, Scope* scope) {
    auto outcome = co_await settle(std::move(task));
    scope->finish_child(std::move(outcome));
}

}  // namespace dftracer::utils::coro
