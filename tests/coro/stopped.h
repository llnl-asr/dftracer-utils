// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_CORO_STOPPED_H
#define DFTRACER_UTILS_TESTS_CORO_STOPPED_H

#include <dftracer/utils/coro/task.h>

#include <utility>

namespace coro_test {

/// True when `task` ended as stopped. A stop reaches the top level of a task
/// that is not awaited through this, so a test that expects one awaits it here.
template <typename T>
dftracer::utils::coro::CoroTask<bool> ended_stopped(
    dftracer::utils::coro::CoroTask<T> task) {
    auto settled = co_await dftracer::utils::coro::settle(std::move(task));
    co_return settled.stopped;
}

}  // namespace coro_test

#endif  // DFTRACER_UTILS_TESTS_CORO_STOPPED_H
