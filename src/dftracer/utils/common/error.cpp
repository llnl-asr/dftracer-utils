// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>

namespace dftracer::utils::common {

dstd::Error error_of_exception(const std::exception_ptr& error) noexcept {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        return dstd::Error{dstd::Condition::Unknown, e.what()};
    } catch (...) {
        return dstd::Error{dstd::Condition::Unknown, "unknown error"};
    }
}

}  // namespace dftracer::utils::common
