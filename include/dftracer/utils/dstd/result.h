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

#define DFTRACER_UTILS_TRY_NAME(line) dftracer_utils_try_##line
#define DFTRACER_UTILS_TRY_TEMP(line) DFTRACER_UTILS_TRY_NAME(line)

/// `DFTRACER_UTILS_TRY(auto value, expr)` evaluates `expr`, an @ref
/// dftracer::utils::dstd::Expected, and declares `value` from its value; when
/// `expr` holds an error it returns that error from the enclosing function,
/// which must return an Expected of the same error type. Use it in a function,
/// not in a coroutine, where `co_await` ends the task the same way.
#define DFTRACER_UTILS_TRY(decl, expr)                               \
    auto DFTRACER_UTILS_TRY_TEMP(__LINE__) = (expr);                 \
    if (!DFTRACER_UTILS_TRY_TEMP(__LINE__).has_value())              \
        return std::move(DFTRACER_UTILS_TRY_TEMP(__LINE__)).error(); \
    decl = *std::move(DFTRACER_UTILS_TRY_TEMP(__LINE__))

/// `DFTRACER_UTILS_TRY_VOID(expr)` for an expression with no value: returns its
/// error from the enclosing function, or goes on.
#define DFTRACER_UTILS_TRY_VOID(expr)                          \
    do {                                                       \
        auto&& dftracer_utils_try_void = (expr);               \
        if (!dftracer_utils_try_void.has_value())              \
            return std::move(dftracer_utils_try_void).error(); \
    } while (false)

#endif  // DFTRACER_UTILS_DSTD_RESULT_H
