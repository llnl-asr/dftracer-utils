// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_CORO_FAILING_H
#define DFTRACER_UTILS_TESTS_CORO_FAILING_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/error.h>

#include <type_traits>
#include <utility>

namespace coro_test {

/// A task that ends with a dstd::Condition::Unknown error holding `message`.
template <typename T = void>
dftracer::utils::coro::CoroTask<T> failing_task(const char* message) {
    dftracer::utils::dstd::Error error{
        dftracer::utils::dstd::Condition::Unknown, message};
    auto failure = dftracer::utils::coro::fail_with(std::move(error));
    co_await failure;
    if constexpr (!std::is_void_v<T>) co_return T{};
}

}  // namespace coro_test

#endif  // DFTRACER_UTILS_TESTS_CORO_FAILING_H
