// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
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

bool Scope::stop_requested() const noexcept {
    if (stop_.load(std::memory_order_acquire)) return true;
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
    request_stop();
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
    if (error) scope->fail(std::move(error));
}

}  // namespace dftracer::utils::coro
