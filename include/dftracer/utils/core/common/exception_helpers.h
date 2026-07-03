#ifndef DFTRACER_UTILS_CORE_COMMON_EXCEPTION_HELPERS_H
#define DFTRACER_UTILS_CORE_COMMON_EXCEPTION_HELPERS_H

#include <exception>
#include <utility>

namespace dftracer::utils {

// Move the stored exception out, clear the slot, then rethrow. Clearing first
// prevents rethrowing the same exception twice if the holder is awaited again.
[[noreturn]] inline void rethrow_and_clear(std::exception_ptr& ex) {
    auto e = std::move(ex);
    ex = nullptr;
    std::rethrow_exception(std::move(e));
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_EXCEPTION_HELPERS_H
