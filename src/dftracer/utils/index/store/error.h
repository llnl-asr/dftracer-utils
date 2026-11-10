// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_STORE_ERROR_H
#define DFTRACER_UTILS_INDEX_STORE_ERROR_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::index::store {

enum class IndexerErrorKind {
    DatabaseError,
    FileError,
    CompressionError,
    InvalidArgument,
    BuildError,
    UnknownError
};

/// An error of `code` with the message "<kind text>: <message>".
dstd::Error indexer_error(IndexerErrorKind kind, dstd::StringView message,
                          common::ErrorCode code = common::ErrorCode::Indexer);

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_ERROR_H
