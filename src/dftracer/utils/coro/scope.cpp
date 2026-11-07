// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/cancelled.h>
#include <dftracer/utils/coro/scope.h>

#include <cstdlib>
#include <exception>
#include <utility>

namespace dftracer::utils::coro {

Scope::~Scope() {
    if (!joined_ && spawned_.load(std::memory_order_acquire)) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "Scope destroyed without join() after spawning");
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

std::exception_ptr Scope::refusal() const {
    if (closed_.load(std::memory_order_acquire)) {
        return std::make_exception_ptr(common::DFTUtilsException(
            common::ErrorCode::Pipeline,
            "Cannot spawn coroutine: scope is closed"));
    }
    if (executor_ == nullptr) {
        return std::make_exception_ptr(common::DFTUtilsException(
            common::ErrorCode::Pipeline,
            "Cannot spawn coroutine: executor is not running"));
    }
    auto token = executor_->shutdown_token();
    if (token && token->load(std::memory_order_acquire)) {
        return std::make_exception_ptr(common::DFTUtilsException(
            common::ErrorCode::Pipeline,
            "Cannot spawn coroutine: executor is not running"));
    }
    return nullptr;
}

bool Scope::admit() {
    std::exception_ptr refused = refusal();
    if (!refused) return true;
    // Counted as spawned, so a refused spawn is still reported by join().
    spawned_.store(true, std::memory_order_release);
    fail(std::move(refused));
    return false;
}

void Scope::fail(std::exception_ptr error) noexcept {
    if (!failed_.exchange(true, std::memory_order_acq_rel)) {
        error_ = std::move(error);
    }
    if (mode_ == ScopeMode::FailFast) request_stop();
}

void Scope::child_failed(std::exception_ptr error) noexcept {
    if (!detail::is_cancelled_error(error)) {
        fail(std::move(error));
        return;
    }
    // The stop that ended this child is not its error, or a sibling's error
    // would lose to it and a plain request_stop would fail the join.
    if (stop_.load(std::memory_order_acquire)) return;
    // Cancelled from outside, such as at shutdown. Join still reports it, but
    // the siblings get their own cancel, and a stop here would also end the
    // ones that are not parked and would have finished.
    if (!failed_.exchange(true, std::memory_order_acq_rel)) {
        error_ = std::move(error);
    }
}

void Scope::start(detail::Coro& child) {
    join_.track(child);
    spawned_.store(true, std::memory_order_release);
    child.handle().promise().executor = executor_;
    executor_->post(child.release());
}

void Scope::finish_join(bool self_join) {
    if (self_join) {
        throw common::DFTUtilsException(
            common::ErrorCode::Pipeline,
            "A task cannot join the scope that spawned it");
    }
    joined_ = true;
    close();
    if (error_) common::rethrow_and_clear(error_);
}

detail::Coro Scope::run_child(CoroTask<void> task, Scope* scope) {
    std::exception_ptr error;
    try {
        co_await std::move(task);
    } catch (...) {
        error = std::current_exception();
    }
    if (error) scope->child_failed(std::move(error));
}

}  // namespace dftracer::utils::coro
