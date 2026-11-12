// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_PY_ERROR_EXCEPTION_H
#define DFTRACER_UTILS_PYTHON_PY_ERROR_EXCEPTION_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

#include <exception>
#include <type_traits>
#include <utility>

namespace dftracer::utils::python {

/// Binding-local exception carrying the dstd::Error of a failed call, so the
/// domain survives until the Python exception type is chosen.
class PyErrorException : public std::exception {
   public:
    PyErrorException(dftracer::utils::common::ErrorCode code,
                     dftracer::utils::dstd::StringView message)
        : error_(code, message) {}
    explicit PyErrorException(const dftracer::utils::dstd::Error &error)
        : error_(error) {}

    const char *what() const noexcept override { return error_.c_str(); }
    const dftracer::utils::dstd::Error &error() const noexcept {
        return error_;
    }

   private:
    dftracer::utils::dstd::Error error_;
};

/// The value of `r`, or throws @ref PyErrorException with its error.
template <typename T>
T py_unwrap(dftracer::utils::dstd::Result<T> &&r) {
    if (!r.has_value()) throw PyErrorException(r.error());
    if constexpr (!std::is_void_v<T>) return std::move(*r);
}

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_PY_ERROR_EXCEPTION_H
