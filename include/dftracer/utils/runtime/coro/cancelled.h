#ifndef DFTRACER_UTILS_RUNTIME_CORO_CANCELLED_H
#define DFTRACER_UTILS_RUNTIME_CORO_CANCELLED_H

#include <dftracer/utils/common/error.h>

namespace dftracer::utils::coro {

/// Throws the error a cancelled wait or cancelled task ends with. Its
/// condition() is dstd::Condition::Cancelled.
[[noreturn]] inline void throw_cancelled(const char* what) {
    throw DFTUtilsException(dstd::Error{dstd::Condition::Cancelled, what});
}

inline bool is_cancelled(const DFTUtilsException& e) noexcept {
    return e.condition() == dstd::Condition::Cancelled;
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_RUNTIME_CORO_CANCELLED_H
