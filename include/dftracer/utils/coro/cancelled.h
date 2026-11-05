// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_CANCELLED_H
#define DFTRACER_UTILS_CORO_CANCELLED_H

#include <dftracer/utils/common/error.h>

#include <exception>

namespace dftracer::utils::coro {

/// Throws the error a cancelled wait or cancelled task ends with. Its
/// condition() is dstd::Condition::Cancelled.
[[noreturn]] inline void throw_cancelled(const char* what) {
    throw common::DFTUtilsException(
        dstd::Error{dstd::Condition::Cancelled, what});
}

inline bool is_cancelled(const common::DFTUtilsException& e) noexcept {
    return e.condition() == dstd::Condition::Cancelled;
}

namespace detail {
bool is_cancelled_error(const std::exception_ptr& error) noexcept;
}  // namespace detail

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_CANCELLED_H
