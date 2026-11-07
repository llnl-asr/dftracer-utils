// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/store/error.h>

namespace dftracer::utils::index::store {
dstd::String IndexerError::format_message(Type type,
                                          const dstd::String &message) {
    const char *prefix = "";
    switch (type) {
        case DatabaseError:
            prefix = "Database error";
            break;
        case FileError:
            prefix = "File error";
            break;
        case CompressionError:
            prefix = "Compression error";
            break;
        case InvalidArgument:
            prefix = "Invalid argument";
            break;
        case BuildError:
            prefix = "Build error";
            break;
        case UnknownError:
            prefix = "Unknown error";
            break;
    }
    return dstd::String(prefix) + ": " + message;
}
}  // namespace dftracer::utils::index::store
