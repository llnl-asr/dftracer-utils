// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_RESULT_H
#define DFTRACER_UTILS_DSTD_RESULT_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>

namespace dftracer::utils::dstd {

/// A value or the @ref Error that kept it from being made. A failure in the
/// ordinary course of use comes back as a Result, not as an exception.
template <class T>
using Result = Expected<T, Error>;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_RESULT_H
