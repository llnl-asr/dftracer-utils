// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_TASK_RESULT_H
#define DFTRACER_UTILS_TESTS_TASK_RESULT_H

// The value of the Result that get(), block_on() and sync_wait() give, for a
// test that expects the task to have succeeded. A test of an error reads the
// Result itself.

#include <dftracer/utils/dstd/result.h>
#include <doctest/doctest.h>

#include <type_traits>
#include <utility>

namespace dftu_utils_test {

template <typename R>
typename std::remove_cvref_t<R>::value_type ok(R&& result) {
    REQUIRE_MESSAGE(result.has_value(), result.error().format().c_str());
    if constexpr (!std::is_void_v<
                      typename std::remove_cvref_t<R>::value_type>) {
        return *std::forward<R>(result);
    }
}

}  // namespace dftu_utils_test

#endif  // DFTRACER_UTILS_TESTS_TASK_RESULT_H
