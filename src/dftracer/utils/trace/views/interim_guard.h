// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_INTERIM_GUARD_H
#define DFTRACER_UTILS_TRACE_VIEWS_INTERIM_GUARD_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/result.h>

#include <exception>
#include <type_traits>
#include <utility>

// The view files that still throw (the aggregation, DuQL and plan-operation
// files) are called from files that return errors. This is the one place that
// turns their exception into an error result. Delete it with the last such
// file.
namespace dftracer::utils::trace::views::detail {

template <class T>
struct IsResult : std::false_type {};
template <class T>
struct IsResult<dstd::Result<T>> : std::true_type {};

/// What `f()` returns, as a result: its own result when it returns one, else
/// the value or void wrapped. An exception of `f` is its error.
template <class F>
auto guard_exceptions(F&& f) {
    using R = std::invoke_result_t<F>;
    using Out = std::conditional_t<IsResult<R>::value, R, dstd::Result<R>>;
    try {
        if constexpr (IsResult<R>::value) {
            return f();
        } else if constexpr (std::is_void_v<R>) {
            f();
            return Out{};
        } else {
            return Out(f());
        }
    } catch (...) {
        return Out(common::error_of_exception(std::current_exception()));
    }
}

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_INTERIM_GUARD_H
