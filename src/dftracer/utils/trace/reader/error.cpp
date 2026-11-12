// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/reader/error.h>

namespace dftracer::utils::trace::reader {

const char *reader_error_prefix(ReaderErrorType type) {
    switch (type) {
        case ReaderErrorType::DatabaseError:
            return "Database error";
        case ReaderErrorType::FileIoError:
            return "File I/O error";
        case ReaderErrorType::CompressionError:
            return "Compression error";
        case ReaderErrorType::InvalidArgument:
            return "Invalid argument";
        case ReaderErrorType::InitializationError:
            return "Initialization error";
        case ReaderErrorType::ReadError:
            return "Read error";
        case ReaderErrorType::UnknownError:
            return "Unknown error";
    }
    return "";
}

dstd::Unexpected<dstd::Error> reader_error(ReaderErrorType type,
                                           dstd::StringView message) {
    return common::make_error<"{}: {}">(
        common::ErrorCode::Reader, dstd::StringView(reader_error_prefix(type)),
        message);
}

}  // namespace dftracer::utils::trace::reader
