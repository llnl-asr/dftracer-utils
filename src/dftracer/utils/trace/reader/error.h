// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_READER_ERROR_H
#define DFTRACER_UTILS_TRACE_READER_ERROR_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::trace::reader {

/// The kind of a reader failure. It sets the prefix of the message; the error
/// code is always @ref common::ErrorCode::Reader.
enum class ReaderErrorType {
    DatabaseError,
    FileIoError,
    CompressionError,
    InvalidArgument,
    InitializationError,
    ReadError,
    UnknownError,
};

/// The prefix of a message of the given kind, such as "Read error".
const char *reader_error_prefix(ReaderErrorType type);

/// An error with code @ref common::ErrorCode::Reader whose message is the
/// prefix of `type`, ": " and `message`. It converts to any dstd::Result<T>.
dstd::Unexpected<dstd::Error> reader_error(ReaderErrorType type,
                                           dstd::StringView message);

/// @ref reader_error with a message built from a format:
///   return reader_error<ReaderErrorType::ReadError, "bad {}">(name);
template <ReaderErrorType T, dstd::FixedString S, dstd::FmtArg... A>
dstd::Unexpected<dstd::Error> reader_error(const A &...a) {
    return reader_error(T, dstd::StringView(dstd::fmt<S>(a...)));
}

}  // namespace dftracer::utils::trace::reader

#endif  // DFTRACER_UTILS_TRACE_READER_ERROR_H
