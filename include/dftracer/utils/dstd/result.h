// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_RESULT_H
#define DFTRACER_UTILS_DSTD_RESULT_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>

#include <utility>

namespace dftracer::utils::dstd {

/// A value or the @ref Error that kept it from being made. A failure in the
/// ordinary course of use comes back as a Result, not as an exception.
template <class T>
using Result = Expected<T, Error>;

}  // namespace dftracer::utils::dstd

#define DFTRACER_UTILS_RESULT_CONCAT_(a, b) a##b
#define DFTRACER_UTILS_RESULT_CONCAT(a, b) DFTRACER_UTILS_RESULT_CONCAT_(a, b)

/// In a coroutine that returns a @ref dftracer::utils::dstd::Result, evaluates
/// `expr`, co_returns its error on failure, and else binds its value to
/// `decl`. Usage: DFTRACER_UTILS_CO_TRY(auto value, co_await make_value());
#define DFTRACER_UTILS_CO_TRY(decl, expr)                                \
    auto DFTRACER_UTILS_RESULT_CONCAT(dftu_try_, __LINE__) = (expr);     \
    if (!DFTRACER_UTILS_RESULT_CONCAT(dftu_try_, __LINE__))              \
        co_return ::dftracer::utils::dstd::Unexpected(                   \
            std::move(DFTRACER_UTILS_RESULT_CONCAT(dftu_try_, __LINE__)) \
                .error());                                               \
    decl = *std::move(DFTRACER_UTILS_RESULT_CONCAT(dftu_try_, __LINE__))

#endif  // DFTRACER_UTILS_DSTD_RESULT_H
