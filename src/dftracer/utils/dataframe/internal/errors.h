// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_ERRORS_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_ERRORS_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/result.h>

namespace dftracer::utils::dataframe {

/// An InvalidArgument error whose message is the format `S` filled with `a...`
/// (`invalid_argument<"{}: bad count">(who)`); converts to any Result.
template <dstd::FixedString S, dstd::FmtArg... A>
dstd::Unexpected<dstd::Error> invalid_argument(const A&... a) {
    return common::make_error<S>(common::ErrorCode::InvalidArgument, a...);
}

/// A NotFound error: a column, aggregate op or name the caller gave that does
/// not exist. The Python bindings raise it as KeyError where an unknown name
/// used to be std::out_of_range.
template <dstd::FixedString S, dstd::FmtArg... A>
dstd::Unexpected<dstd::Error> not_found(const A&... a) {
    return common::make_error<S>(common::ErrorCode::NotFound, a...);
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_ERRORS_H
