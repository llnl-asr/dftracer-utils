// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/utilities/reader/error.h>

namespace dftracer::utils::utilities::reader {
dstd::String ReaderError::format_message(Type type,
                                         const dstd::String &message) {
    const char *prefix = "";
    switch (type) {
        case ReaderError::DatabaseError:
            prefix = "Database error";
            break;
        case ReaderError::FileIoError:
            prefix = "File I/O error";
            break;
        case ReaderError::CompressionError:
            prefix = "Compression error";
            break;
        case ReaderError::InvalidArgument:
            prefix = "Invalid argument";
            break;
        case ReaderError::InitializationError:
            prefix = "Initialization error";
            break;
        case ReaderError::ReadError:
            prefix = "Read error";
            break;
        case ReaderError::UnknownError:
            prefix = "Unknown error";
            break;
    }
    return dstd::String(prefix) + ": " + message;
}

}  // namespace dftracer::utils::utilities::reader
