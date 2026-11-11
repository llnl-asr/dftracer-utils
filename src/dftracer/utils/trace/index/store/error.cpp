// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/store/error.h>

namespace dftracer::utils::trace::index::store {
dstd::Error indexer_error(IndexerErrorKind kind, dstd::StringView message,
                          common::ErrorCode code) {
    const char *prefix = "";
    switch (kind) {
        case IndexerErrorKind::DatabaseError:
            prefix = "Database error";
            break;
        case IndexerErrorKind::FileError:
            prefix = "File error";
            break;
        case IndexerErrorKind::CompressionError:
            prefix = "Compression error";
            break;
        case IndexerErrorKind::InvalidArgument:
            prefix = "Invalid argument";
            break;
        case IndexerErrorKind::BuildError:
            prefix = "Build error";
            break;
        case IndexerErrorKind::UnknownError:
            prefix = "Unknown error";
            break;
    }
    return dstd::Error::from_format<"{}: {}">(code, prefix, message);
}
}  // namespace dftracer::utils::trace::index::store
