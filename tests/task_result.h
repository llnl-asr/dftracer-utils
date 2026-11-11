// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_TASK_RESULT_H
#define DFTRACER_UTILS_TESTS_TASK_RESULT_H

// The value of the Result that get(), block_on() and sync_wait() give, for a
// test that expects the task to have succeeded. A test of an error reads the
// Result itself.

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/result.h>
#include <doctest/doctest.h>

#include <string_view>
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

// The object a Result of a non-null pointer points to.
template <typename R>
auto& ok_ref(R&& result) {
    return *ok(std::forward<R>(result));
}

// Checks that a Result is an error of the given code, and when `text` is not
// null that its message is exactly `text`.
template <typename R>
void expect_error(const R& result, dftracer::utils::common::ErrorCode code,
                  const char* text = nullptr) {
    REQUIRE_MESSAGE(!result.has_value(), "expected an error result");
    CHECK(dftracer::utils::common::errorcode_of(result.error()) == code);
    if (text != nullptr)
        CHECK(std::string_view(result.error().c_str()) == text);
}

}  // namespace dftu_utils_test

#endif  // DFTRACER_UTILS_TESTS_TASK_RESULT_H
