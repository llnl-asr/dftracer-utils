// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_GENERATOR_OK_H
#define DFTRACER_UTILS_TESTS_GENERATOR_OK_H

// A generator of Result items as a generator of their values, for a test that
// expects every item to succeed. An error item fails the test through ok().
// The value of an item stays valid until the next pull, as for the source.

#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/dstd/result.h>
#include <task_result.h>

#include <utility>

namespace dftu_utils_test {

template <typename T>
dftracer::utils::coro::AsyncGenerator<T> ok_items(
    dftracer::utils::coro::AsyncGenerator<dftracer::utils::dstd::Result<T>>
        source) {
    for (;;) {
        auto next = co_await source.next();
        if (!next) co_return;
        co_yield ok(std::move(*next));
    }
}

}  // namespace dftu_utils_test

#endif  // DFTRACER_UTILS_TESTS_GENERATOR_OK_H
